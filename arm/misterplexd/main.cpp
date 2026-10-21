// misterplexd — ARM-side daemon for MiSTerPlex.
// Phase 2: GDM + companion + FFmpeg → /dev/fb0 (FPGA scanout via MiSTer_fb).
// Phase 4: multi-server conf, auto next-episode, optional subtitle burn-in.

#include "companion.hpp"
#include "natural_eof.hpp"
#include "playback_controls.hpp"
#include "gcast/airplay_fe.hpp"
#include "gcast/yt_fe.hpp"
#include "gcast/cast_resolve.hpp"
#include "gcast/cast_glass.hpp"
#include "gcast/yt_dlp.hpp"
#include "gcast/dash_seek.hpp"
#include "gcast/protected_src.hpp"
#include "gcast/session_arbiter.hpp"
#include "wall_clock.hpp"
#include "media_player.hpp"
#include "pms_timeline.hpp"
#include "plex_resolve.hpp"
#include "seek_reuse.hpp"
#include "queue_step.hpp"
#include "transcode_offset.hpp"
#include "libmisterplex/gcast_osd.hpp"
#include "libmisterplex/display_raster.hpp"
#include "libmisterplex/av_clock.hpp"
#include "libmisterplex/core_gate.hpp"
#include "libmisterplex/owenb_pms_traits.hpp"
#include "libmisterplex/daemon_signal.hpp"
#include "libmisterplex/d52_log.hpp"
#include "libmisterplex/h264_filter_setting.hpp"
#include "libmisterplex/legacy_core_prefix.hpp"
#include "libmisterplex/browse_v2.hpp"
#include "libmisterplex/pms_reach.hpp"
#include "libmisterplex/status_heartbeat.hpp"
#include "log_redact.hpp"
#include "libmisterplex/plexui/plexui_config.hpp"
#include "lock_paths.hpp"
#include "mem_stat.hpp"
#include "lab_endpoints.hpp"
#include <functional>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <fstream>
#include <ctime>
#include <mutex>
#include <string>
#include <pthread.h>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};
std::atomic<bool> g_signalHold{false};
std::atomic<int> g_doPlayDepth{0};
std::atomic<bool> g_termHeld{false};
std::atomic<bool> g_termHoldLogged{false};
void on_signal(int sig) {
    if (sig == SIGTERM &&
        !misterplex::daemonTermShouldExit(g_signalHold.load(std::memory_order_acquire),
                                           g_termHeld.load(std::memory_order_acquire))) {
        g_termHeld.store(true, std::memory_order_release);
        return;
    }
    g_stop.store(true);
}
#ifdef MPX_HAVE_MAST_RELOAD
// MAST-WEDGE: set once the guarded-reload launcher has started; main exits with
// misterplex::mast::kReloadExitCode so the launcher's load finds no daemon. The hand-off time
// lets the exit line report the teardown (review 21:52: it must end well inside the
// launcher's MR_EXIT_WAIT).
std::atomic<bool> g_mastReload{false};
std::atomic<int64_t> g_mastReloadAtMs{0};
int64_t mastSteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
int64_t mastTeardownMs() { return mastSteadyMs() - g_mastReloadAtMs.load(); }
#endif

class FpgaWorkerHandoff {
public:
    explicit FpgaWorkerHandoff(misterplex::MediaPlayer& player) : player_(player) {
        player_.suspendFpgaWorkers();
    }
    ~FpgaWorkerHandoff() {
        if (!completed_)
            player_.resumeFpgaWorkers(true);
    }
    FpgaWorkerHandoff(const FpgaWorkerHandoff&) = delete;
    FpgaWorkerHandoff& operator=(const FpgaWorkerHandoff&) = delete;

    void completePlayback() {
        player_.resumeFpgaWorkers(false);
        completed_ = true;
    }

private:
    misterplex::MediaPlayer& player_;
    bool completed_ = false;
};

std::string loadConf(const std::string& path, const char* key) {
    std::ifstream in(path);
    if (!in)
        return {};
    const std::string p = std::string(key) + "=";
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        if (line.rfind(p, 0) == 0)
            return line.substr(p.size());
    }
    return {};
}

// Collect every KEY= value (for multi-line PLEX_BASE=).
std::vector<std::string> loadConfAll(const std::string& path, const char* key) {
    std::vector<std::string> out;
    std::ifstream in(path);
    if (!in)
        return out;
    const std::string p = std::string(key) + "=";
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        if (line.rfind(p, 0) == 0)
            out.push_back(line.substr(p.size()));
    }
    return out;
}

// Upsert KEY=value in conf (preserves comments/other keys). Used so F12 OSD
// content/display choices persist into conf the user can inspect.
bool upsertConfKey(const std::string& path, const char* key, const std::string& value) {
    if (path.empty() || !key || !*key)
        return false;
    std::ifstream in(path);
    std::vector<std::string> lines;
    std::string line;
    bool found = false;
    const std::string p = std::string(key) + "=";
    if (in) {
        while (std::getline(in, line)) {
            if (!found && line.rfind(p, 0) == 0) {
                lines.push_back(p + value);
                found = true;
            } else {
                lines.push_back(line);
            }
        }
    }
    if (!found)
        lines.push_back(p + value);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
            return false;
        for (const auto& l : lines)
            out << l << '\n';
    }
    return ::rename(tmp.c_str(), path.c_str()) == 0;
}

bool confTruthy(const std::string& v) {
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

misterplex::WeakLadder weakForContentResolution(const misterplex::WeakLadder& base,
                                                const misterplex::ContentResolution& res,
                                                bool bitrateExplicit,
                                                bool qualityExplicit,
                                                bool h264ProfileExplicit) {
    misterplex::WeakLadder weak = base;
    // Prefer named profile so level/bitrate stay consistent with tier.
    // applyPlexTranscodeProfile always rewrites maxVideoBitrateKbps / videoQuality /
    // h264Profile from the named ladder (e.g. 720p→main@20M@q100). Preserve operator
    // WEAK_BITRATE / WEAK_QUALITY / WEAK_H264_PROFILE when set so the light present-rate
    // ladder can stick (baseline is cheaper to decode on dual-A9).
    const int explicitBitrateKbps = base.maxVideoBitrateKbps;
    const int explicitQuality = base.videoQuality;
    const std::string explicitH264Profile = base.h264Profile;
    const std::string geomRes =
        std::to_string(res.width) + "x" + std::to_string(res.height);
    // Prefer named product label (240p/480p/720p), then WxH geometry.
    if (!misterplex::applyPlexTranscodeProfile(res.label, weak) &&
        !misterplex::applyPlexTranscodeProfile(geomRes, weak)) {
        weak.profileName = res.label;
        // PMS universal wants WxH in videoResolution=, never the short label alone.
        weak.videoResolution = geomRes;
        if (res.width >= 1280 || res.height >= 720) {
            weak.h264Level = 31;
            if (!qualityExplicit)
                weak.videoQuality = 95;
        }
    }
    if (bitrateExplicit)
        weak.maxVideoBitrateKbps = explicitBitrateKbps;
    else
        weak.maxVideoBitrateKbps = res.weakBitrateKbps;
    if (qualityExplicit && explicitQuality >= 1 && explicitQuality <= 100)
        weak.videoQuality = explicitQuality;
    if (h264ProfileExplicit && (explicitH264Profile == "baseline" || explicitH264Profile == "main"))
        weak.h264Profile = explicitH264Profile;
    return weak;
}

} // namespace

namespace {

// MiSTerPlex ships its own static ffmpeg, but installs that predate that (or that
// share a box with mistercast-linux) keep it elsewhere. Probe our own bin first so
// a stock install is self-contained, then fall back rather than hard-failing.
std::string defaultFfmpegPath() {
    for (const char* c : {"/media/fat/misterplex/bin/ffmpeg", "/media/fat/mistercast/bin/ffmpeg"}) {
        if (::access(c, X_OK) == 0)
            return c;
    }
    return "/media/fat/misterplex/bin/ffmpeg";
}

// One process only. A second copy fights DDR banks and corrupts glass.
// Hold the fd for the process lifetime. Do not unlink the path (same
// inode rule as /tmp/misterplex_spi.lock).
class ProcessSingleton {
public:
    static inline const char* const kPath = misterplex::daemonLockPath();
    static constexpr int kExitAlreadyRunning = 3;

    bool acquire(std::string& err) {
        fd_ = ::open(kPath, O_CREAT | O_RDWR, 0666);
        if (fd_ < 0) {
            err = std::string("open ") + kPath + " failed: " + std::strerror(errno);
            return false;
        }
        if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            char buf[32]{};
            ::lseek(fd_, 0, SEEK_SET);
            const ssize_t n = ::read(fd_, buf, sizeof(buf) - 1);
            int holder = 0;
            if (n > 0)
                holder = std::atoi(buf);
            err = "already running";
            if (holder > 0)
                err += " pid=" + std::to_string(holder);
            err += std::string(" lock=") + kPath;
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        if (::ftruncate(fd_, 0) != 0) {
            err = std::string("ftruncate ") + kPath + " failed: " + std::strerror(errno);
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        char pidbuf[32];
        const int len =
            std::snprintf(pidbuf, sizeof(pidbuf), "%d\n", static_cast<int>(::getpid()));
        if (len <= 0 || ::write(fd_, pidbuf, static_cast<size_t>(len)) != len) {
            err = std::string("write pid ") + kPath + " failed: " + std::strerror(errno);
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        return true;
    }

    ~ProcessSingleton() {
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
    }

    ProcessSingleton() = default;
    ProcessSingleton(const ProcessSingleton&) = delete;
    ProcessSingleton& operator=(const ProcessSingleton&) = delete;

private:
    int fd_ = -1;
};

} // namespace

int main(int argc, char** argv) {
    // The rescue thread is joinable. Every return after it starts, including
    // companion-start failure and the lab play-file exits, has to join it or
    // the process abort with SIGABRT (exit 134).
    struct StopWallClockOnReturn {
        ~StopWallClockOnReturn() { misterplex::stopWallClockRescue(); }
    };
    [[maybe_unused]] StopWallClockOnReturn stopWallClockOnReturn;
    // Keep comm=misterplexd so killall -x misterplexd matches. mpx-main hid
    // every extra copy from killall and let a daemon storm paint DDR.

    std::string name = "MiSTerPlex";
    std::string machineId = "misterplex-1";
    int port = 3005;
    std::string ffmpeg = defaultFfmpegPath();
    std::string confPath = "/media/fat/misterplex/misterplex.conf";
    std::string confToken;
    std::string plexTvToken;
    bool plexTvLinkEnabled = true;
    int decodeW = 320, decodeH = 240;
    bool cliDecodeExplicit = false;
    int cliDecodeW = 0, cliDecodeH = 0;
    std::string confDecodeValue;
    std::string presentMode = "fb0";
    misterplex::DdrFrameFormat ddrFrameFormat = misterplex::DdrFrameFormat::Yuv420p;
    bool ddrMemSync = true;
    bool ddrMemFlush = false;
    bool presentProfile = false;
    bool streamEnabled = false;
    std::string streamSkipRgb = "auto"; // auto | on | off — skip heavy RGB when PRESENT=fpga
    bool autoNext = true;
    std::string subtitleMode = "off"; // off | burn | ffmpeg
    int subtitleStreamId = -1;
    // Phase 4 match-source-Hz: conf reserved for switchres; Content FPS hint is software-only.
    std::string matchSourceHz = "off";
    std::string sourceFpsConf = "auto";
    misterplex::WeakLadder weak;
    std::string cliTranscodeProfile;
    bool transcodeProfileExplicit = false;
    bool weakResExplicit = false;
    bool weakBitrateExplicit = false;
    bool weakQualityExplicit = false;
    bool weakH264ProfileExplicit = false;
    std::vector<std::string> servers;
    std::string defaultPms;
    int64_t skipForwardMs = 30000;
    int64_t skipBackMs = 10000;
    // Lab: --play-file PATH [--play-seconds N] [--play-count N] plays a local file N times in
    // this process (default once), then exits (no GDM). PATH may be an http URL.
    // --play-duration-ms N declares the PMS duration (default: play-seconds; 0 = unknown) and
    // --play-start-ms N the resume offset, as a cast passes them (N2 end-of-stream tests).
    std::string playFile;
    int playSeconds = 25;
    int playCount = 1;
    long long playDurationMs = -1;
    long long playStartMs = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--name") == 0 && i + 1 < argc)
            name = argv[++i];
        else if (std::strcmp(argv[i], "--id") == 0 && i + 1 < argc)
            machineId = argv[++i];
        else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--ffmpeg") == 0 && i + 1 < argc)
            ffmpeg = argv[++i];
        else if (std::strcmp(argv[i], "--pms") == 0 && i + 1 < argc)
            defaultPms = argv[++i];
        else if (std::strcmp(argv[i], "--conf") == 0 && i + 1 < argc)
            confPath = argv[++i];
        else if (std::strcmp(argv[i], "--decode") == 0 && i + 1 < argc) {
            int w = 0, h = 0;
            if (std::sscanf(argv[++i], "%dx%d", &w, &h) == 2) {
                decodeW = w;
                decodeH = h;
                cliDecodeExplicit = true;
                cliDecodeW = w;
                cliDecodeH = h;
            }
        } else if (std::strcmp(argv[i], "--transcode-profile") == 0 && i + 1 < argc) {
            cliTranscodeProfile = argv[++i];
        } else if (std::strcmp(argv[i], "--play-file") == 0 && i + 1 < argc) {
            playFile = argv[++i];
        } else if (std::strcmp(argv[i], "--play-seconds") == 0 && i + 1 < argc) {
            playSeconds = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--play-count") == 0 && i + 1 < argc) {
            playCount = std::max(1, std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--play-duration-ms") == 0 && i + 1 < argc) {
            playDurationMs = std::max<long long>(0, std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--play-start-ms") == 0 && i + 1 < argc) {
            playStartMs = std::max<long long>(0, std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("misterplexd [--name N] [--id ID] [--port N] [--ffmpeg PATH] [--pms URL] "
                        "[--conf PATH] [--decode WxH] [--transcode-profile 240p|480p] "
                        "[--play-file PATH] [--play-seconds N] [--play-count N] "
                        "[--play-duration-ms N] [--play-start-ms N]\n");
            return 0;
        }
    }

    ProcessSingleton singleton;
    {
        std::string lockErr;
        if (!singleton.acquire(lockErr)) {
            std::fprintf(stderr, "misterplexd: %s — one copy only (DDR/glass)\n",
                         lockErr.c_str());
            return ProcessSingleton::kExitAlreadyRunning;
        }
    }

    {
        // Multi-server: PLEX_SERVERS=url1,url2 and/or repeated PLEX_BASE= lines.
        auto baseLines = loadConfAll(confPath, "PLEX_BASE");
        auto serversCsv = loadConf(confPath, "PLEX_SERVERS");
        servers = misterplex::mergePlexServers(serversCsv, baseLines);
        auto host = loadConf(confPath, "PLEX_HOST");
        if (!host.empty()) {
            auto hbase = misterplex::normalizePlexBase(host);
            if (!hbase.empty()) {
                bool seen = false;
                for (const auto& s : servers) {
                    if (s == hbase) {
                        seen = true;
                        break;
                    }
                }
                if (!seen)
                    servers.insert(servers.begin(), hbase);
            }
        }
        if (!servers.empty())
            defaultPms = servers.front();
        else {
            auto n = misterplex::normalizePlexBase(defaultPms);
            if (!n.empty()) {
                defaultPms = n;
                servers.push_back(n);
            }
        }

        confToken = loadConf(confPath, "PLEX_TOKEN");
        plexTvToken = loadConf(confPath, "PLEXTV_TOKEN");
        // Clock rescue runs on its own thread so a down network at boot does not
        // consume the one shot. Order: rtc0, then SNTP, then HTTPS Date, with
        // backoff until the clock is plausible. YouTube and TLS wait on that thread.
        misterplex::startWallClockRescue();
        {
            auto link = loadConf(confPath, "PLEXTV_LINK");
            if (!link.empty())
                plexTvLinkEnabled = confTruthy(link);
        }
        // FRIENDLY_NAME or auto MiSTerPlex-<lanIp> so WiFi boxes are not "127.0.0.1".
        {
            auto friendly = loadConf(confPath, "FRIENDLY_NAME");
            if (!friendly.empty())
                name = friendly;
            else if (name == "MiSTerPlex") {
                // Defer actual IP until companion exists; mark for post-comp fill.
                name = "MiSTerPlex";
            }
        }
        auto profile = loadConf(confPath, "TRANSCODE_PROFILE");
        if (profile.empty())
            profile = loadConf(confPath, "WEAK_PROFILE");
        if (!profile.empty()) {
            transcodeProfileExplicit = true;
            if (!misterplex::applyPlexTranscodeProfile(profile, weak))
                std::fprintf(stderr, "misterplexd: unknown TRANSCODE_PROFILE=%s (keeping %s)\n",
                             profile.c_str(), weak.profileName.c_str());
        }
        // PMS client profile name: fixed MiSTerPlex on every request; conf CLIENT_PROFILE_NAME or env
        // MPX_CLIENT_PROFILE_NAME (wins) overrides it for a server whose XML has another name.
        if (const std::string pn = loadConf(confPath, "CLIENT_PROFILE_NAME"); !pn.empty())
            misterplex::setClientProfileNameConf(pn);
        // NO-REQUIRED-PROFILE: no profile name unless SEND_PROFILE_NAME=fixed|variant (or CLIENT_PROFILE_NAME).
        if (const std::string sp = loadConf(confPath, "SEND_PROFILE_NAME"); !sp.empty())
            misterplex::setSendProfileNameConf(sp);
        std::fprintf(stderr, "misterplexd: PMS client profile name: %s\n",
                     misterplex::profileNameSendDescription().c_str());
        std::fprintf(stderr, "misterplexd: PMS profile Extra: %s\n",
                     misterplex::plexSendProfileExtra()
                         ? "send=extra (env MPX_SEND_PROFILE_EXTRA)"
                         : "send=none (no MPX_SEND_PROFILE_EXTRA; Name/Extra opt-in only)");
        {
            std::string src;
            const std::string pn = misterplex::baselineClientProfileName(&src);
            std::fprintf(stderr, "misterplexd: client profile name '%s' (%s)\n", pn.c_str(), src.c_str());
        }
        auto v = loadConf(confPath, "FFMPEG");
        if (!v.empty())
            ffmpeg = v;
        // TRUE640 A/B: conf FPGA_480P_CODED_W=640 opts in to 640-coded 480p.
        // An explicit MPX_FPGA_480P_CODED_W env always wins (=624 forces 624).
        v = loadConf(confPath, "FPGA_480P_CODED_W");
        if (!v.empty() && !std::getenv("MPX_FPGA_480P_CODED_W"))
            setenv("MPX_FPGA_480P_CODED_W", v.c_str(), 0);
        v = loadConf(confPath, "DECODE");
        if (!v.empty()) {
            confDecodeValue = v;
            int w = 0, h = 0;
            if (std::sscanf(v.c_str(), "%dx%d", &w, &h) == 2) {
                decodeW = w;
                decodeH = h;
            }
        }
        v = loadConf(confPath, "WEAK_RES");
        if (!v.empty()) {
            weakResExplicit = true;
            if (!misterplex::applyPlexTranscodeProfile(v, weak)) {
                weak.profileName = "custom";
                weak.videoResolution = v;
            }
        }
        v = loadConf(confPath, "WEAK_BITRATE");
        if (!v.empty()) {
            weakBitrateExplicit = true;
            weak.maxVideoBitrateKbps = std::atoi(v.c_str());
        }
        v = loadConf(confPath, "WEAK_QUALITY");
        if (!v.empty()) {
            const int q = std::atoi(v.c_str());
            if (q >= 1 && q <= 100) {
                weak.videoQuality = q;
                weakQualityExplicit = true;
            }
        }
        // Lab present-rate ladder: WEAK_H264_PROFILE=baseline (cheaper dual-A9 decode).
        // Default 720p named profile is main (quality path); baseline may band more.
        v = loadConf(confPath, "WEAK_H264_PROFILE");
        if (v.empty())
            v = loadConf(confPath, "H264_PROFILE");
        if (!v.empty()) {
            if (v == "baseline" || v == "main") {
                weak.h264Profile = v;
                weakH264ProfileExplicit = true;
            } else {
                std::fprintf(stderr,
                             "misterplexd: WEAK_H264_PROFILE=%s ignored (use baseline|main)\n",
                             v.c_str());
            }
        }
        v = loadConf(confPath, "PRESENT");
        if (!v.empty())
            presentMode = v; // fb0 | fpga | both | none(test/lab)
        v = loadConf(confPath, "DDR_FRAME_FORMAT");
        if (!v.empty() && v != "yuv420p" && v != "yuv420" && v != "i420") {
            std::fprintf(stderr,
                         "misterplexd: DDR_FRAME_FORMAT=%s ignored; DDR frame store is "
                         "fixed to yuv420p\n",
                         v.c_str());
        }
        v = loadConf(confPath, "DDR_MEM_SYNC");
        if (!v.empty())
            ddrMemSync = confTruthy(v);
        v = loadConf(confPath, "DDR_MEM_FLUSH");
        if (!v.empty())
            ddrMemFlush = confTruthy(v);
        v = loadConf(confPath, "PRESENT_PROFILE");
        if (!v.empty())
            presentProfile = confTruthy(v);
        // Env wins over conf so official play can set PRESENT_PROFILE=1
        // while the living box conf stays 0.
        if (const char* e = std::getenv("PRESENT_PROFILE")) {
            if (e[0])
                presentProfile = confTruthy(e);
        }
        v = loadConf(confPath, "STREAM");
        if (!v.empty())
            streamEnabled = confTruthy(v);
        v = loadConf(confPath, "STREAM_SKIP_RGB");
        if (!v.empty())
            streamSkipRgb = v;
        v = loadConf(confPath, "AUTO_NEXT");
        if (!v.empty())
            autoNext = confTruthy(v);
        // Phase 4 subtitles: off | burn (PMS universal) | ffmpeg (local files, STREAM=0)
        v = loadConf(confPath, "SUBTITLES");
        if (!v.empty()) {
            subtitleMode = v;
            if (v == "burn" || v == "1" || v == "true" || v == "yes" || v == "on") {
                weak.burnSubtitles = true;
                subtitleMode = (v == "ffmpeg") ? "ffmpeg" : "burn";
            } else if (v == "ffmpeg") {
                subtitleMode = "ffmpeg";
            } else {
                subtitleMode = "off";
            }
        }
        v = loadConf(confPath, "SUBTITLE_STREAM");
        if (!v.empty()) {
            subtitleStreamId = std::atoi(v.c_str());
            weak.subtitleStreamId = subtitleStreamId;
        }
        // Phase 4: match-source-Hz / Content FPS (see docs/match-source-hz.md).
        // Conf is applied on each play: SOURCE_FPS selects Content FPS hint from PMS
        // metadata (or forces 12/24/30/60). MATCH_SOURCE_HZ=on still cannot switch
        // modelines without HPS switchres — logs target only.
        v = loadConf(confPath, "MATCH_SOURCE_HZ");
        if (!v.empty())
            matchSourceHz = v;
        v = loadConf(confPath, "SOURCE_FPS");
        if (!v.empty())
            sourceFpsConf = v;
        v = loadConf(confPath, "SKIP_MS");
        if (!v.empty()) {
            const int ms = std::atoi(v.c_str());
            if (ms >= 0) {
                skipForwardMs = ms;
                skipBackMs = ms;
            }
        }
        v = loadConf(confPath, "SKIP_FORWARD_MS");
        if (!v.empty())
            skipForwardMs = std::max(0, std::atoi(v.c_str()));
        v = loadConf(confPath, "SKIP_BACK_MS");
        if (!v.empty())
            skipBackMs = std::max(0, std::atoi(v.c_str()));
        std::fprintf(stderr,
                     "misterplexd: MATCH_SOURCE_HZ=%s SOURCE_FPS=%s "
                     "(cadence/OSD path; switchres TODO)\n",
                     matchSourceHz.c_str(), sourceFpsConf.c_str());
    }
    // CLI_DECODE_WINS: persist DECODE= (OSD 240p) must not clobber --decode.
    // T7 --decode 1280x720 + DECODE=320x240 polled PLXJ at 0x3007F130, not L4.
    if (cliDecodeExplicit) {
        if (decodeW != cliDecodeW || decodeH != cliDecodeH) {
            std::fprintf(stderr,
                         "misterplexd: CLI_DECODE_WINS --decode %dx%d "
                         "(conf DECODE=%s ignored)\n",
                         cliDecodeW, cliDecodeH,
                         confDecodeValue.empty() ? "(unset)" : confDecodeValue.c_str());
        } else {
            std::fprintf(stderr, "misterplexd: CLI_DECODE_WINS --decode %dx%d\n",
                         cliDecodeW, cliDecodeH);
        }
        decodeW = cliDecodeW;
        decodeH = cliDecodeH;
    }
    if (!cliTranscodeProfile.empty()) {
        transcodeProfileExplicit = true;
        if (!misterplex::applyPlexTranscodeProfile(cliTranscodeProfile, weak))
            std::fprintf(stderr, "misterplexd: unknown --transcode-profile=%s (keeping %s)\n",
                         cliTranscodeProfile.c_str(), weak.profileName.c_str());
    }
    // Align PMS ladder with decode size only when the operator did not choose a
    // transcode profile/resolution explicitly. This is the fallback path when
    // OSD_CONTROL=0 or no Plex core is loaded; with OSD_CONTROL=1 the OSD status
    // word (O[4]) is the content-resolution source of truth.
    if (!transcodeProfileExplicit && !weakResExplicit &&
        weak.videoResolution == "320x240" && (decodeW != 320 || decodeH != 240)) {
        const std::string decodeRes = std::to_string(decodeW) + "x" + std::to_string(decodeH);
        if (!misterplex::applyPlexTranscodeProfile(decodeRes, weak)) {
            // Named profile miss: still publish a valid weak ladder for 720p custom.
            if (decodeW >= 1280 || decodeH >= 720) {
                if (!misterplex::applyPlexTranscodeProfile("720p", weak)) {
                    weak.profileName = "720p";
                    weak.videoResolution = "1280x720";
                    weak.h264Level = 31;
                    if (!weakBitrateExplicit)
                        weak.maxVideoBitrateKbps = 4000;
                }
            } else {
                weak.profileName = "custom";
                weak.videoResolution = decodeRes;
                if (!weakBitrateExplicit) {
                    if (decodeW >= 640)
                        weak.maxVideoBitrateKbps = 2500;
                    else if (decodeW >= 480)
                        weak.maxVideoBitrateKbps = 1500;
                }
            }
        }
    }
    std::string weakWhy;
    if (!misterplex::validateWeakLadder(weak, &weakWhy)) {
        std::fprintf(stderr, "misterplexd: invalid transcode profile (%s); falling back to 240p\n",
                     weakWhy.c_str());
        misterplex::applyPlexTranscodeProfile("240p", weak);
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGCHLD, SIG_DFL);
    // Session handoff (seek / new playMedia) calls killChildren() while the audio
    // and STREAM pump threads may still be writing to the ffmpeg pipes. The default
    // SIGPIPE action terminates the process *silently* — no log line, no dmesg entry
    // — and it is not in installCrashGuard()'s list, so Main is left SIGSTOPped and
    // F12/OSD die with us. Ignoring it turns those writes into a normal EPIPE that
    // the pump loops already treat as end-of-stream.
    std::signal(SIGPIPE, SIG_IGN);
    // P6-MEMORY lab telemetry: MPX_MEMSTAT_SEC=<n> logs mallinfo2 every n s. Unset = off.
    if (const int memstatSec = misterplex::mem_stat::startFromEnv())
        std::fprintf(stderr, "misterplexd: MEMSTAT on, every %d s\n", memstatSec);

    // An SPI critical section SIGSTOPs Main for a few microseconds. If a previous
    // misterplexd died inside that window, Main is still stopped right now and
    // F12/OSD/MiSTer_cmd are all dead — resume it before we do anything else,
    // then arm the crash guard so we cannot strand it again.
    misterplex::FpgaSpi::resumeStrandedMain();
    misterplex::FpgaSpi::installCrashGuard();

    const char* shellFilter = std::getenv("MPX_H264_FILTER"); // before the conf loop overrides it
    const std::string shellFilterValue = shellFilter ? shellFilter : "";
    const bool shellFilterSet = shellFilter != nullptr;
    // Read before the conf loop. MPX_LEGACY_CORE_PREFIX is not in that loop:
    // a deployed misterplex.conf still names retired prod RBF 7cbae324, and
    // that line used to replace boot.sh. The compiled RC prefix is the pin.
    const char* bootLegacy = std::getenv("MPX_LEGACY_CORE_PREFIX");
    const std::string bootLegacyValue = bootLegacy ? bootLegacy : "";
    for (const char* key : {"MPX_VIDEO_BACKEND",
                            "MPX_H264_PROTOTYPE", "MPX_H264_FILTER", "MPX_GATE_ERROR_SCREEN",
                            "MPX_PRESENT_480I", "MPX_FABRIC_DIRECT",
                            "MPX_STICK_I420", "MPX_FPGA_MODE"}) {
        const auto value = loadConf(confPath, key);
        // Conf wins over stale shell env so mode-pair deploys are deterministic.
        if (!value.empty())
            ::setenv(key, value.c_str(), 1);
    }
    {
        const auto confLegacy = loadConf(confPath, "MPX_LEGACY_CORE_PREFIX");
        const bool bootDiffers = !bootLegacyValue.empty() &&
                                 bootLegacyValue != misterplex::kLegacyCorePrefix;
        const bool confDiffers = !confLegacy.empty() && confLegacy != misterplex::kLegacyCorePrefix;
        if (bootDiffers || confDiffers) {
            std::fprintf(stderr,
                         "misterplexd: MPX_LEGACY_CORE_PREFIX pinned to %s "
                         "(conf '%s', boot '%s' ignored)\n",
                         misterplex::kLegacyCorePrefix,
                         confLegacy.c_str(), bootLegacyValue.c_str());
        }
        ::setenv("MPX_LEGACY_CORE_PREFIX", misterplex::kLegacyCorePrefix, 1);
    }
    std::fprintf(stderr, "%s\n",
                 misterplex::h264FilterLogLine(misterplex::effectiveH264Filter(
                     loadConf(confPath, "MPX_H264_FILTER"), shellFilterSet ? shellFilterValue.c_str() : nullptr))
                     .c_str());
    // rc2 LAN endpoints (lab_endpoints.hpp): MPX_LAB_HOST (env or conf) points ladder :18767, mpx-menu :18769
    // and gcast-xcode :18780 at one LAN helper; per-service env keys win. Derived values are exported so the
    // existing readers (plex_resolve, cast_resolve, browse) see them unchanged.
    {
        namespace le = misterplex::lab_endpoints;
        const le::Endpoints ep = le::resolve(
            [](const char* k) -> std::optional<std::string> {
                const char* v = std::getenv(k);
                return v ? std::optional<std::string>(v) : std::nullopt;
            },
            [&confPath](const char* k) -> std::optional<std::string> {
                const auto v = loadConf(confPath, k);
                return v.empty() ? std::nullopt : std::optional<std::string>(v);
            });
        if (ep.ladderSrc == "MPX_LAB_HOST" || ep.ladderSrc.rfind("env MPX_LADDER_URL", 0) == 0)
            ::setenv("MPX_REMOTE_LADDER_PROXY", ep.ladder.c_str(), 1);
        if (ep.menuSrc == "MPX_LAB_HOST" || ep.menuSrc == "conf MPX_MENU_URL")
            ::setenv("MPX_MENU_URL", ep.menu.c_str(), 1);
        if (ep.xcodeSrc == "MPX_LAB_HOST" || ep.xcodeSrc == "conf GCAST_XCODE")
            ::setenv("GCAST_XCODE", ep.xcode.c_str(), 1);
        std::fprintf(stderr, "%s\n", le::logLine(ep).c_str());
    }
    // Lab FFmpeg ladder: explicit env only. Unset or empty = direct PMS (Goal C).
    // Do not load conf REMOTE_LADDER_PROXY: that silently re-enables the lab-pc proxy.
    if (const char* p = std::getenv("MPX_REMOTE_LADDER_PROXY"); p && p[0])
        std::fprintf(stderr, "misterplexd: REMOTE_LADDER_PROXY=%s (debug opt-in)\n", p);
    else
        std::fprintf(stderr, "misterplexd: REMOTE_LADDER_PROXY unset (direct PMS)\n");
    // DISPLAY/CONTENT 480i ⇒ present-only R15 bank (720×480 / 0x301FF000).
    if (!std::getenv("MPX_PRESENT_480I")) {
        const auto dlab = loadConf(confPath, "DISPLAY_RES");
        const auto clab = loadConf(confPath, "CONTENT_RES");
        if (dlab == "480i" || clab == "480i")
            ::setenv("MPX_PRESENT_480I", "1", 0);
    }
    misterplex::MediaPlayer player;
    // An INT between menu.rbf and PlexOB leaves CORENAME on Menu. Reload it
    // before the idle loop, and again on the way out if this process is the
    // one that got the signal mid-bounce.
    player.recoverStrandedMenuCore(true);
    misterplex::mem_stat::setQueueProbe([&player] {
        misterplex::mem_stat::QueueDepths q;
        player.owenbQueueDepths(q.out, q.ready, q.probe);
        return q;
    });
#ifdef MPX_HAVE_MAST_RELOAD
    player.setMastReloadHandler([](const std::string&) {
        g_mastReloadAtMs.store(mastSteadyMs());
        g_mastReload.store(true);
        g_stop.store(true);
    });
#endif
    // MPX_BROWSE_V2 (browse_v2.hpp): read once. Off (unset/""/"0") prints nothing.
    const char* browseV2Raw = std::getenv(misterplex::kBrowseV2Env);
    const misterplex::BrowseV2Setting browseV2Setting = misterplex::parseBrowseV2(browseV2Raw);
    const std::string browseMenuUrl = misterplex::menuUrlFromEnvironment(
        std::getenv(misterplex::kMenuUrlEnv), std::getenv(misterplex::kMenuBaseEnv));
    // MVP-b (PLXI commands 6-11) only with MPX_BROWSE_V2=1 and MPX_BROWSE_MVPB=1.
    const bool browseNavCommands =
        browseV2Setting == misterplex::BrowseV2Setting::On &&
        misterplex::parseBrowseV2(std::getenv(misterplex::kBrowseNavEnv)) ==
            misterplex::BrowseV2Setting::On;
    {
        const std::string notice = misterplex::formatBrowseV2Notice(
            browseV2Setting, browseV2Raw, browseMenuUrl, browseNavCommands);
        if (!notice.empty())
            std::fprintf(stderr, "%s\n", notice.c_str());
    }
    const bool plexUiOn = misterplex::plexui::plexUiEnabled(browseV2Setting ==
                                                                misterplex::BrowseV2Setting::On,
                                                            browseNavCommands);
    {
        const std::string plexUiNotice =
            misterplex::plexui::formatPlexUiNotice(plexUiOn);
        if (!plexUiNotice.empty())
            std::fprintf(stderr, "%s\n", plexUiNotice.c_str());
    }
    // The browse play hook, set once doPlay exists (below); the worker calls it.
    std::mutex browsePlayMu;
    std::function<uint64_t(const misterplex::BrowsePlayTicket&)> browsePlay;
    player.setFfmpegPath(ffmpeg);
    {
        // Lab present-rate ladder: FFMPEG_SWS_FLAGS=fast_bilinear|exact_fast_bilinear|…
        // Default remains bicubic for soft-sky banding (see SCORE_BANDING_FIX).
        auto sws = loadConf(confPath, "FFMPEG_SWS_FLAGS");
        if (!sws.empty()) {
            player.setFfmpegSwsFlags(sws);
            std::fprintf(stderr, "misterplexd: FFMPEG_SWS_FLAGS=%s\n", sws.c_str());
        }
        auto fpsf = loadConf(confPath, "FFMPEG_FPS_FILTER");
        if (!fpsf.empty()) {
            const bool on = confTruthy(fpsf); // off/0/false → omit fps= filter
            player.setFfmpegFpsFilter(on);
            std::fprintf(stderr, "misterplexd: FFMPEG_FPS_FILTER=%s\n", on ? "on" : "off");
        }
        // DEINTERLACE=yadif|bwdif|bob|off — host bob for interlaced sources on
        // the 480i host-paint path (DVD bob lesson without RBF field raster yet).
        auto deint = loadConf(confPath, "DEINTERLACE");
        if (deint.empty())
            deint = loadConf(confPath, "MPX_DEINTERLACE");
        if (!deint.empty()) {
            player.setDeinterlaceMode(deint);
            std::fprintf(stderr, "misterplexd: DEINTERLACE=%s\n", deint.c_str());
        } else {
            const auto dlab = loadConf(confPath, "DISPLAY_RES");
            const auto clab = loadConf(confPath, "CONTENT_RES");
            // Default bob on 480i host path: cheap insurance for interlaced TV.
            // Progressive film still passes (yadif deint=interlaced).
            if (dlab == "480i" || clab == "480i") {
                player.setDeinterlaceMode("yadif");
                std::fprintf(stderr, "misterplexd: DEINTERLACE=yadif (default for 480i)\n");
            }
        }
        // UV bias: counter fluorescent green (measured U low on HDMI vs source).
        int uBias = 0, vBias = 0;
        auto ub = loadConf(confPath, "UV_U_BIAS");
        auto vb = loadConf(confPath, "UV_V_BIAS");
        if (!ub.empty())
            uBias = std::atoi(ub.c_str());
        if (!vb.empty())
            vBias = std::atoi(vb.c_str());
        if (uBias != 0 || vBias != 0) {
            player.setUvBias(uBias, vBias);
            std::fprintf(stderr, "misterplexd: UV_U_BIAS=%d UV_V_BIAS=%d\n", uBias, vBias);
        }
    }
    // Core gate (core_gate.hpp): closed at start. First look at the loaded core before
    // any FPGA access; the gate opens on a later tick, once the core answers a CAPS
    // probe with an allowlisted build (no glass probe, idle paint or polls before).
    if (misterplex::CoreGate::process().managed()) {
        std::fprintf(stderr,
                     "misterplexd: core gate closed at start: no FPGA traffic until the loaded "
                     "core answers CAPS with an allowlisted build\n");
        // Own-core: pin only when MPX_FPGA_MODE is set. Otherwise leave unpaired so
        // the first recognised CAPS raster (480p / 480i / 240p) pairs the daemon.
        const char* modeEnv = std::getenv("MPX_FPGA_MODE");
        if (modeEnv && *modeEnv) {
            const std::string mode = misterplex::fpgaModeForLadder(weak);
            misterplex::CoreGate::process().setPairedRaster(
                mode, "FPGA mode " + mode + ": MPX_FPGA_MODE=" + std::string(modeEnv));
        } else {
            std::fprintf(stderr,
                         "misterplexd: core gate unpaired at start; first CAPS raster pairs "
                         "(own-core 480i/240p)\n");
        }
    }
    player.coreGateTick();
    // MIX-GLASS-MAX: probe live doorbell page once. true480 never 1280x720.
    // Pin 960-store prefix8 so OSD Content 720p cannot snap idle back to 1280.
    {
        const auto pin = misterplex::readLiveGlassPin();
        const auto glass = misterplex::CoreGate::process().open()
                               ? misterplex::probeLiveGlass()
                               : misterplex::probeLiveGlass("");
        player.setLiveGlass(glass);
        // pin.prefix8 hashes _Utility/Plex.rbf on disk, which need not be the loaded core.
        std::string prefix8 = pin.prefix8;
        const char* prefixSource = "disk _Utility/Plex.rbf";
        if (prefix8.empty()) {
            if (const char* leg = std::getenv("MPX_LEGACY_CORE_PREFIX")) {
                prefix8 = leg;
                prefixSource = "env MPX_LEGACY_CORE_PREFIX";
            }
        }
        if (!prefix8.empty())
            std::fprintf(stderr, "misterplexd: disk RBF prefix8=%s (%s; not the loaded core)\n",
                         prefix8.c_str(), prefixSource);
        player.setRbfPrefix8(prefix8);
        const auto bank = misterplex::glassMax(decodeW, decodeH, glass, pin.prefix8);
        const uint32_t db = misterplex::presentBankDoorbell(bank.width, bank.height);
        if (bank.width != decodeW || bank.height != decodeH) {
            std::fprintf(stderr,
                         "misterplexd: GLASS-MAX glass=%s content=%dx%d → bank=%dx%d "
                         "doorbell=0x%08X\n",
                         misterplex::liveGlassLabel(glass), decodeW, decodeH, bank.width,
                         bank.height, db);
        } else {
            std::fprintf(stderr,
                         "misterplexd: GLASS-MAX glass=%s bank=%dx%d doorbell=0x%08X\n",
                         misterplex::liveGlassLabel(glass), bank.width, bank.height, db);
        }
        // Keep conf/--decode as the PMS ladder. true480 glassMax bank is 640x480
        // present-only; overwriting DECODE here made 882b/true24 cores request
        // 624x480 AUs that blow the 8KiB ring (CAPS max 320x240).
        std::fprintf(stderr,
                     "misterplexd: content ladder remains DECODE %dx%d (present bank %dx%d)\n",
                     decodeW, decodeH, bank.width, bank.height);
    }
    player.setDecodeSize(decodeW, decodeH);
    {
        // Display raster latches here (daemon start = core reset). Live F12
        // Display only upserts DISPLAY_RES; next start applies it.
        const auto dlab = loadConf(confPath, "DISPLAY_RES");
        const misterplex::ContentResolution dr = misterplex::resolutionFromLabel(
            dlab.empty() ? "720p" : dlab.c_str());
        if (misterplex::displayRasterApplyOnStartup()) {
            player.latchAndApplyDisplayRaster(dr);
            std::fprintf(stderr,
                         "misterplexd: DISPLAY_RES=%s latched at start "
                         "(video_mode; F12 Display waits for reset)\n",
                         dr.label);
        }
    }
    player.setPresentMode(presentMode);
    player.setDdrFrameFormat(ddrFrameFormat);
    player.setDdrMemSync(ddrMemSync);
    player.setDdrMemFlush(ddrMemFlush);
    player.setPresentProfile(presentProfile);
    player.setStreamEnabled(streamEnabled);
    player.setStreamSkipRgb(streamSkipRgb);
    player.setSkipDeltasMs(skipForwardMs, skipBackMs);
    std::fprintf(stderr, "misterplexd: SKIP_FORWARD_MS=%lld SKIP_BACK_MS=%lld\n",
                 static_cast<long long>(skipForwardMs), static_cast<long long>(skipBackMs));
    if (subtitleMode == "ffmpeg")
        player.setSubtitleMode("ffmpeg");
    if (subtitleStreamId >= 0)
        player.setSubtitleStreamIndex(subtitleStreamId);
    bool osdControl = false;
    {
        auto audio = loadConf(confPath, "AUDIO");
        if (!audio.empty())
            player.setAudioEnabled(confTruthy(audio));
        auto audioDev = loadConf(confPath, "AUDIO_DEVICE");
        if (!audioDev.empty())
            player.setAudioPath(audioDev);
        // Default 0 — no hardcoded audio lag. Conf AUDIO_DELAY_MS only.
        int audioDelayMs = 0;
        auto adv = loadConf(confPath, "AUDIO_DELAY_MS");
        if (!adv.empty())
            audioDelayMs = std::atoi(adv.c_str());
        player.setAudioDelayMs(audioDelayMs);
        std::fprintf(stderr, "misterplexd: AUDIO_DELAY_MS=%d (0=fresh, no hardcoded lag)\n",
                     audioDelayMs);
    }
    {
        // A/V pacing knobs. Defaults match the shipped behaviour; conf lets the lab
        // retune without a rebuild.
        auto lead = loadConf(confPath, "AV_PRESENT_LEAD_MS");
        if (!lead.empty())
            player.setPresentLeadMs(std::atoi(lead.c_str()));
        else
            player.setPresentLeadMs(misterplex::kDefaultPresentLeadMs);
        auto hdmiLag = loadConf(confPath, "AV_HDMI_AUDIO_LAG_MS");
        if (!hdmiLag.empty())
            player.setAvHdmiAudioLagMs(std::atoi(hdmiLag.c_str()));
        auto drop = loadConf(confPath, "AV_RESYNC_DROP_MS");
        if (!drop.empty())
            player.setResyncDropMs(std::atoi(drop.c_str()));
        else
            player.setResyncDropMs(misterplex::MediaPlayer::kDefaultResyncDropMs);
        auto ppm = loadConf(confPath, "AUDIO_CLOCK_PPM");
        if (!ppm.empty())
            player.setAudioClockPpm(std::atoi(ppm.c_str()));
        std::fprintf(stderr, "misterplexd: AUDIO_CLOCK_PPM=%d\n", player.audioClockPpm());
        auto avoff = loadConf(confPath, "AV_OFFSET_MS");
        if (!avoff.empty())
            player.setAvOffsetMs(std::atoi(avoff.c_str()));
        std::fprintf(stderr, "misterplexd: AV_PRESENT_LEAD_MS=%s AV_RESYNC_DROP_MS=%s\n",
                     lead.empty() ? "40(default)" : lead.c_str(),
                     drop.empty() ? "80(default)" : drop.c_str());
        std::fprintf(stderr, "misterplexd: AV_HDMI_AUDIO_LAG_MS=%s\n",
                     hdmiLag.empty() ? "auto" : hdmiLag.c_str());
    }
    {
        // Idle/screensaver: without this the last frame of the previous video stays
        // latched in the frame store after playback ends.
        auto idle = loadConf(confPath, "IDLE_SCREEN");
        misterplex::IdleMode im = misterplex::IdleMode::Logo;
        std::string idleLabel = idle.empty() ? "logo(default)" : idle;
        if (idle == "black")
            im = misterplex::IdleMode::Black;
        else if (idle == "screensaver")
            im = misterplex::IdleMode::Screensaver;
        else if (idle == "last" || idle == "lastframe")
            im = misterplex::IdleMode::LastFrame;
        else if (idle == "off") {
            // Historical alias for last-frame stickiness; product glass must show
            // the chevron. Keep explicit last/lastframe for deliberate lab use.
            std::fprintf(stderr,
                         "misterplexd: IDLE_SCREEN=off is deprecated (was last-frame); "
                         "using logo. Set IDLE_SCREEN=last only if sticky last frame is intended.\n");
            im = misterplex::IdleMode::Logo;
            idleLabel = "logo(from-off)";
        } else if (!idle.empty() && idle != "logo") {
            std::fprintf(stderr,
                         "misterplexd: unknown IDLE_SCREEN=%s — using logo\n", idle.c_str());
            im = misterplex::IdleMode::Logo;
            idleLabel = "logo(fallback)";
        }
        player.setIdleMode(im);
        // OSD_CONTROL requires the v7 CONF_STR layout; on an older core the same
        // bits mean Pattern/Content FPS and would decode as a bogus A/V offset.
        osdControl = confTruthy(loadConf(confPath, "OSD_CONTROL"));
        player.setOsdControl(osdControl);
        std::fprintf(stderr, "misterplexd: OSD_CONTROL=%s\n", osdControl ? "1" : "0");
        std::fprintf(stderr, "misterplexd: IDLE_SCREEN=%s AV_OFFSET_MS=%d\n",
                     idleLabel.c_str(), player.avOffsetMs());
    }
    player.setLog([](const std::string& s) { misterplex::emitLogSink(s); });
    (void)misterplex::d52::instance(); // D52-LOG (lab, MPX_D52_LOG=1): the start line; nothing when off
    if (streamEnabled) {
        std::fprintf(stderr,
                     "misterplexd: STREAM=1 (annex-B → host I-recon F1 + F3; preferDirectH264; "
                     "PRESENT=%s STREAM_SKIP_RGB=%s — skip RGB only when PRESENT=fpga)\n",
                     presentMode.c_str(), streamSkipRgb.c_str());
    }
    std::fprintf(stderr, "misterplexd: DDR_MEM_SYNC=%s DDR_MEM_FLUSH=%s\n",
                 ddrMemSync ? "1" : "0", ddrMemFlush ? "1" : "0");
    std::fprintf(stderr, "misterplexd: DDR_FRAME_FORMAT=yuv420p\n");
    std::fprintf(stderr, "misterplexd: PRESENT_PROFILE=%s\n", presentProfile ? "1" : "0");
    if (weak.burnSubtitles)
        std::fprintf(stderr, "misterplexd: SUBTITLES=burn (PMS universal)\n");
    else if (subtitleMode == "ffmpeg")
        std::fprintf(stderr, "misterplexd: SUBTITLES=ffmpeg (local files, STREAM=0)\n");
    if (!player.initPresent()) {
        std::fprintf(stderr, "misterplexd: WARNING no present path — companion only\n");
    } else {
        // MPX_BROWSE_V2=1 (lab, browse_v2.hpp): the browser's worker, fetch and paint path
        // must be in place before the first paint and the polls. Unset: nothing happens here.
        if (browseV2Setting == misterplex::BrowseV2Setting::On) {
            const auto browsePlayHook = [&](const misterplex::BrowsePlayTicket& t) -> uint64_t {
                std::function<uint64_t(const misterplex::BrowsePlayTicket&)> hook;
                {
                    std::lock_guard<std::mutex> lk(browsePlayMu);
                    hook = browsePlay;
                }
                return hook ? hook(t) : 0;
            };
            if (plexUiOn) {
                misterplex::plexui::PlexUiController::Config pc;
                pc.accountToken =
                    misterplex::plexui::plexUiAccountToken(confToken);
                pc.fontDir = misterplex::plexui::plexUiDirFromEnvironment();
                pc.homeServer = loadConf(confPath, "PLEX_HOME");
                player.enablePlexUi(pc, browsePlayHook);
                std::fprintf(stderr, "misterplexd: plexui token=%s fonts=%s\n",
                             pc.accountToken.empty() ? "(none)" : "(set)",
                             pc.fontDir.c_str());
            } else {
                misterplex::BrowseController::Config bc;
                bc.menuUrl = browseMenuUrl;
                bc.pmsBase = defaultPms;
                bc.pmsToken = confToken;
                const auto bg = misterplex::ddrFrameGeometryForPresentedSize(player.decodeW(),
                                                                           player.decodeH());
                bc.cols = misterplex::browseTitleCols(bg.coded_width);
                bc.rows = misterplex::libraryRowsOnScreen(bg.coded_width, bg.coded_height);
                player.enableBrowseV2(bc, browsePlayHook, browseNavCommands);
                std::fprintf(stderr, "misterplexd: browse v2 helper=%s cols=%d rows=%d pms=%s\n",
                             bc.menuUrl.c_str(), bc.cols, bc.rows,
                             defaultPms.empty() ? "(none)" : defaultPms.c_str());
            }
        }
        if (misterplex::librarylist::browseLibListEnabled(std::getenv(misterplex::librarylist::kBrowseLibListEnv))) {
            player.setLibraryListEnabled(true);
            std::fprintf(stderr,
                         "misterplexd: LAB MPX_BROWSE_LIBLIST=1 -> home sections+items on the idle FB "
                         "(624x480)\n");
        }
        // Paint immediately at boot, then keep the slow idle safety net running.
        // startIdle alone left a window where the latched last frame remained on glass.
        player.paintIdle();
        player.startIdle();
        player.startOsdPoll();
        player.startInputPoll();
    }

    // Lab A/V sync: play local file and exit (no companion / GDM).
    if (!playFile.empty()) {
        // The main loop's gate tick is not running yet: let the gate settle and verify
        // the loaded core first (core_gate.hpp), or the play is refused.
        if (!player.waitCoreGateOpen(5000)) {
            std::fprintf(stderr, "misterplexd: play-file failed: core gate closed (loaded core "
                                 "not verified within 5 s)\n");
            return 1;
        }
#ifdef MPX_HAVE_MAST_RELOAD
        if (g_mastReload.load()) {
            std::fprintf(stderr, "misterplexd: play-file: MAST auto core reload handed off at "
                                 "gate open; exiting %d (teardown %lld ms since the hand-off)\n",
                         misterplex::mast::kReloadExitCode,
                         static_cast<long long>(mastTeardownMs()));
            return misterplex::mast::kReloadExitCode;
        }
#endif
        // Restore CLI/conf canvas after OSD seed. decodeW/H are already
        // glass-capped (true480 never 1280x720). Do not skip ACK.
        player.setDecodeSize(decodeW, decodeH);
        {
            const auto geom = misterplex::ddrFrameGeometryForPresentedSize(decodeW, decodeH);
            const auto layout = misterplex::makeDdrFrameLayout(
                geom, misterplex::ddrFramePhysBaseForGeometry(geom));
            // PLXJ = page + 0x130 (mailbox_abi::kPlxjOff). A literal on purpose:
            // test_n2_eof_e2e N2_EOF_BASE_REF builds this main.cpp in pre-kPlx trees.
            const uint32_t plxjPhys = layout.doorbell_phys + 0x130u;
            std::fprintf(stderr,
                         "misterplexd: LAB play-file=%s seconds=%d decode=%dx%d "
                         "doorbell=0x%08X PLXJ=0x%08X%s\n",
                         playFile.c_str(), playSeconds, decodeW, decodeH,
                         layout.doorbell_phys, plxjPhys,
                         (decodeW == 1280 && decodeH == 720) ? " L4" : "");
        }
        std::string probeFail;
        int srcW = 0, srcH = 0;
        int srcFpsN = 0, srcFpsD = 0;
        const auto sourceAspect =
            player.probeSourceAspect(playFile, {}, &probeFail, &srcW, &srcH,
                                     &srcFpsN, &srcFpsD);
        if (srcW > 0 && srcH > 0) {
            player.setSourceMediaSize(srcW, srcH);
            std::fprintf(stderr, "misterplexd: play-file source_coded=%dx%d\n", srcW,
                         srcH);
        }
        if (srcFpsN > 0 && srcFpsD > 0) {
            misterplex::capExactFpsToMaxHz(srcFpsN, srcFpsD, 30.0);
            player.setContentFpsRational(srcFpsN, srcFpsD);
            std::fprintf(stderr,
                         "misterplexd: play-file content fps=%d/%d (match cap 30Hz)\n",
                         srcFpsN, srcFpsD);
        }
        // A lab play-file plays its own file: never a transcode (review 19:37 N1).
        player.setSourceTranscoded(false);
        if ((!sourceAspect.valid && !player.fpgaH264Backend()) ||
            !player.setSourceAspect(sourceAspect)) {
            if (!sourceAspect.valid) {
                std::fprintf(stderr,
                             "misterplexd: play-file probe: source aspect invalid (%s)\n",
                             probeFail.empty() ? "unknown" : probeFail.c_str());
            }
            std::fprintf(stderr,
                         "misterplexd: play-file failed: source display aspect unavailable\n");
            return 1;
        }
        // Lab --play-count N: N plays of the file in this one process, as a cast's next play
        // would run on the same daemon (a retained epoch is this process's state).
        int rc = 0;
        for (int run = 1; run <= playCount; ++run) {
            if (playCount > 1)
                std::fprintf(stderr, "misterplexd: LAB play-file run %d/%d\n", run, playCount);
            if (!player.play(playFile, playStartMs, {},
                             playDurationMs >= 0 ? playDurationMs : playSeconds * 1000LL)) {
                std::fprintf(stderr, "misterplexd: play-file failed\n");
                rc = 1;
                continue;
            }
            // Wait up to playSeconds; exit early only after we have observed playing
            // then see it clear (natural EOF). Do not treat the pre-thread window as done.
            bool sawPlaying = false;
            for (int i = 0; i < playSeconds * 10; ++i) {
                if (player.playing())
                    sawPlaying = true;
                else if (sawPlaying)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            player.stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            const auto summary = player.lastPlaybackSummary();
#ifdef MPX_HAVE_MAST_RELOAD
            if (g_mastReload.load()) {
                std::fprintf(stderr, "misterplexd: LAB play-file run %d: MAST auto core reload "
                                     "handed off at begin; exiting %d (teardown %lld ms since "
                                     "the hand-off, incl. the session's teardown)\n",
                             run, misterplex::mast::kReloadExitCode,
                             static_cast<long long>(mastTeardownMs()));
                return misterplex::mast::kReloadExitCode;
            }
#endif
            if (summary.deliveredFrames() <= 0) {
                std::fprintf(stderr,
                             "misterplexd: LAB play-file ERROR zero frames delivered "
                             "(raw=%lld presented=%lld recon=%lld totalBytes=%lld short_read=%d "
                             "got=%zu/%zu eof=%d stream=%d skip_rgb=%d). Check FFmpeg stderr/log, "
                             "source streams, and DDR/F1 delivery before grading captures.\n",
                             static_cast<long long>(summary.rawFrames),
                             static_cast<long long>(summary.presentedFrames),
                             static_cast<long long>(summary.reconFrames),
                             static_cast<long long>(summary.totalBytes), summary.shortRead ? 1 : 0,
                             summary.shortReadGot, summary.shortReadWant, summary.videoEof ? 1 : 0,
                             summary.streamEnabled ? 1 : 0, summary.skipRgb ? 1 : 0);
                rc = 2;
                continue;
            }
            std::fprintf(stderr,
                         "misterplexd: LAB play-file done frames=%lld presented=%lld recon=%lld "
                         "totalBytes=%lld\n",
                         static_cast<long long>(summary.rawFrames),
                         static_cast<long long>(summary.presentedFrames),
                         static_cast<long long>(summary.reconFrames),
                         static_cast<long long>(summary.totalBytes));
            rc = 0;
        }
        return rc;
    }

    misterplex::Companion comp;
    // WiFi-only MiSTers often have no default route → old lanIp was 127.0.0.1.
    // Append real LAN IP so Plex cast UI never shows loopback as the player name.
    if (name == "MiSTerPlex") {
        const std::string ip = comp.localLanIp();
        if (!ip.empty() && ip.rfind("127.", 0) != 0)
            name = "MiSTerPlex-" + ip;
    }
    comp.setName(name);
    player.setObIdleName(name);   // OWENB-P1B T7: the OB idle card shows the cast name
    player.owenbFbconAtStart();   // OWENB-P1B 0009: attach a console a dead daemon left detached (G15)
    comp.setMachineId(machineId);
    comp.setPort(static_cast<uint16_t>(port));
    comp.setLog([](const std::string& s) { misterplex::emitLogSink(s); });
    comp.setPlexTvEnabled(plexTvLinkEnabled);
    comp.setPlexTvToken(plexTvToken);
    comp.setPlexTvLinkPath("/media/fat/misterplex/plex_link.txt");
    comp.setPlexTvPersist([&](const std::string& tok) {
        if (!upsertConfKey(confPath, "PLEXTV_TOKEN", tok))
            std::fprintf(stderr, "misterplexd: failed to persist PLEXTV_TOKEN\n");
    });

    misterplex::PmsTimelineReporter pmsTimeline;
    // BUG1 (lab 7AE78D80): per-PMS memory of what its transcoder emitted for the PlexOB core.
    misterplex::ObPmsTraits obTraits;
    std::mutex obTraitsMu;
    std::string obTraitsKey;   // the current OB play's PMS machineIdentifier when PMS transcodes it, else ""
    const std::string obTraitsPath = [] {
        const char* p = std::getenv("MPX_OB_PMS_TRAITS");
        return std::string(p && *p ? p : "/media/fat/misterplex/ob_pms_traits");
    }();
    obTraits.load(obTraitsPath);
    pmsTimeline.setLog([](const std::string& s) { misterplex::emitLogSink(s); });

    // Session context for multi-base resolve + auto-next.
    std::mutex sessionMu;
    misterplex::PlayRequest lastPlay;
    // The committed remote universal session a seek may replay at a new offset (seek_reuse.hpp).
    misterplex::SeekReuseEntry seekReuse;
    // SPS-gate L3: the request doPlay last committed to the player, and its play generation (sessionMu).
    misterplex::PlayRequest committedPlay;
    uint64_t committedPlayGen = 0;
    std::string lastBase = defaultPms;
    std::string lastToken = confToken;
    std::atomic<bool> autoNextInFlight{false};
    // Monotonic play generation: supersede in-flight async resolve when a newer
    // playMedia/auto-next arrives (P4-SCRUB out-of-order bind race).
    std::atomic<uint64_t> playGen{0};
    std::atomic<uint64_t> activePlayGen{0};
    std::atomic<uint64_t> seekGen{0};
    misterplex::TransportGenerations transport{playGen, seekGen};
    // Serializes stop -> layout/DAR commit -> demux start. FpgaSpi layout changes
    // remap shared DDR and must never race an active presenter or a newer request.
    std::mutex playHandoffMu;

    auto contentResolutionForNextPlay = [&]() -> misterplex::ContentResolution {
        misterplex::ContentResolution raw =
            osdControl ? misterplex::contentResolutionFromOsdWord(player.lastOsdWord())
                       : misterplex::contentResolutionFromSize(decodeW, decodeH);
        // Conf CONTENT_RES wins over DECODE-size collapse when OSD is off so
        // 480i host-paint keeps the 720×480 ladder (not 640×480 "480p").
        if (!osdControl) {
            const auto clab = loadConf(confPath, "CONTENT_RES");
            if (!clab.empty()) {
                const auto labeled = misterplex::resolutionFromLabel(clab.c_str());
                if (clab == "480i" || clab == labeled.label)
                    raw = labeled;
            }
        }
        const auto clamped =
            misterplex::clampContentToLiveGlass(raw, player.liveGlass());
        if (clamped.width != raw.width || clamped.height != raw.height)
            std::fprintf(stderr,
                         "misterplexd: OSD content=%s ignored on glass=%s — using %s "
                         "(RBF present bank, not F12)\n",
                         raw.label, misterplex::liveGlassLabel(player.liveGlass()),
                         clamped.label);
        return clamped;
    };
    auto displayResolutionForNextPlay = [&]() -> misterplex::ContentResolution {
        // HDMI raster follows the live bitstream. F12 Display 240p/720p on
        // true480 fights the 30 Hz 640×480 beam and breaks present.
        misterplex::ContentResolution raw =
            osdControl ? misterplex::displayResolutionFromOsdWord(
                             player.lastOsdWord(),
                             misterplex::contentResolutionFromOsdWord(player.lastOsdWord()))
                       : misterplex::contentResolutionFromSize(decodeW, decodeH);
        if (!osdControl) {
            const auto dlab = loadConf(confPath, "DISPLAY_RES");
            if (!dlab.empty())
                raw = misterplex::resolutionFromLabel(dlab.c_str());
        }
        return misterplex::clampDisplayToLiveGlass(player.liveGlass(), raw);
    };
    auto persistOsdResToConf = [&](const misterplex::ContentResolution& content,
                                   const misterplex::ContentResolution& display) {
        // DECODE= glass-capped bank. TRANSCODE_PROFILE stays content (PMS).
        const auto bank =
            misterplex::glassMax(content.width, content.height, player.liveGlass());
        const std::string cGeom =
            std::to_string(bank.width) + "x" + std::to_string(bank.height);
        if (!upsertConfKey(confPath, "DECODE", cGeom))
            std::fprintf(stderr, "misterplexd: conf upsert DECODE failed path=%s\n",
                         confPath.c_str());
        if (!upsertConfKey(confPath, "TRANSCODE_PROFILE", content.label))
            std::fprintf(stderr, "misterplexd: conf upsert TRANSCODE_PROFILE failed\n");
        if (!upsertConfKey(confPath, "DISPLAY_RES", display.label))
            std::fprintf(stderr, "misterplexd: conf upsert DISPLAY_RES failed\n");
        if (!upsertConfKey(confPath, "CONTENT_RES", content.label))
            std::fprintf(stderr, "misterplexd: conf upsert CONTENT_RES failed\n");
        decodeW = bank.width;
        decodeH = bank.height;
        std::fprintf(stderr,
                     "misterplexd: conf synced from OSD content=%s display=%s DECODE=%s "
                     "TRANSCODE_PROFILE=%s glass=%s bank=%dx%d\n",
                     content.label, display.label, cGeom.c_str(), content.label,
                     misterplex::liveGlassLabel(player.liveGlass()), bank.width, bank.height);
    };

    auto resolveAgainstServers = [&](const misterplex::PlayRequest& req,
                                     const std::string& preferredBase, int64_t off,
                                     const misterplex::WeakLadder& weakForPlay, int matchW,
                                     int matchH) -> std::pair<misterplex::ResolveResult, std::string> {
        // Cast-pinned host must not authenticate with conf token (different PMS).
        std::string token;
        if (!req.token.empty())
            token = req.token;
        else if (req.address.empty())
            token = confToken;
        // Cast-selected base wins when address present.
        std::string selected =
            misterplex::buildPlexBase(req.protocol, req.address, req.port, preferredBase);
        if (selected.empty())
            selected = preferredBase.empty() ? defaultPms : preferredBase;
        // Static ARM libav has no HTTPS. Map cast https/*.plex.direct onto an
        // openable HTTP base — but do NOT force every remote shared server onto
        // conf PLEX_BASE (that 404s foreign ratingKeys like home library shows).
        const std::string lanPreferred =
            !preferredBase.empty() ? preferredBase : defaultPms;
        const std::string openable =
            misterplex::companionOpenablePmsBase(selected, lanPreferred);
        if (!openable.empty() && openable != selected) {
            std::fprintf(stderr,
                         "misterplexd: cast base %s → openable %s (no companion HTTPS)\n",
                         selected.c_str(), openable.c_str());
            selected = openable;
        } else if (openable.empty()) {
            // Foreign private home LAN (cast address is a private LAN address while conf is lab):
            // look up public HTTP via plex.tv using the cast token + machine id.
            // Never rewrite foreign ratingKeys onto conf PLEX_BASE.
            std::string pub;
            // plex.tv /resources needs the *user* token (conf). Cast token is
            // often a server-scoped share token that 401s on plex.tv.
            const std::string lookupTok = !confToken.empty() ? confToken : token;
            std::fprintf(stderr,
                         "misterplexd: foreign-private cast lookup machine=%s "
                         "lookupTok=%zu castTok=%zu confTok=%zu\n",
                         req.serverMachineId.empty() ? "-" : req.serverMachineId.c_str(),
                         lookupTok.size(), token.size(), confToken.size());
            if (!req.serverMachineId.empty() && !lookupTok.empty()) {
                pub = misterplex::companionPublicPmsBaseFromResources(req.serverMachineId,
                                                                      lookupTok);
            }
            if (!pub.empty()) {
                std::fprintf(stderr,
                             "misterplexd: cast base %s → public %s (foreign private LAN)\n",
                             selected.c_str(), pub.c_str());
                selected = pub;
            } else if (!lanPreferred.empty() &&
                       (selected.find("172-") != std::string::npos ||
                        selected.find("172.") != std::string::npos)) {
                // Docker-bridge plex.direct only — conf is the local PMS rewrite.
                std::fprintf(stderr,
                             "misterplexd: cast base %s → LAN/conf %s (unreachable/no IP)\n",
                             selected.c_str(), lanPreferred.c_str());
                selected = lanPreferred;
            } else {
                std::fprintf(stderr,
                             "misterplexd: cast base %s has no openable HTTP path "
                             "(foreign private LAN without public resource lookup)\n",
                             selected.c_str());
            }
        }

        auto tryBase = [&](const std::string& base) -> misterplex::ResolveResult {
            // STREAM=1: prefer direct H.264 Part for CAVLC host recon.
            // STREAM=0: weak universal by default. matchW/H are CONTENT bank for
            // direct-Part size match (not display present size).
            return misterplex::resolvePlayTarget(req.key, base, token, off, /*weakAlways=*/true,
                                                 weakForPlay,
                                                 /*preferDirectH264=*/streamEnabled, matchW,
                                                 matchH, req.forceLadder, req.gateDetail);
        };

        // 720p24: Plex Web pins address=LAN PMS. That 401s the conf token and
        // skips the host 9324 proxy (x86 1280x720 baseline @ 24p). Cast then
        // played testsrc (120s) or PMS HEVC→720 (~13 unique). Prefer proxy.
        if (player.liveGlass() == misterplex::LiveGlass::L4 &&
            preferredBase.find("127.0.0.1:9324") != std::string::npos) {
            std::string t = token;
            if (t.empty())
                t = confToken;
            auto proxied = misterplex::resolvePlayTarget(
                req.key, preferredBase, t, off, /*weakAlways=*/true, weakForPlay,
                /*preferDirectH264=*/streamEnabled, matchW, matchH, req.forceLadder, req.gateDetail);
            if (proxied.ok) {
                std::fprintf(stderr,
                             "misterplexd: 720p24 PLEX_BASE proxy %s (cast address=%s)\n",
                             preferredBase.c_str(),
                             req.address.empty() ? "-" : req.address.c_str());
                return {proxied, preferredBase};
            }
        }

        auto resolved = tryBase(selected);
        if (resolved.ok)
            return {resolved, selected};

        // Multi-server fallback only when cast did not pin an address.
        if (!req.address.empty())
            return {resolved, selected};

        for (const auto& s : servers) {
            if (s == selected)
                continue;
            auto r = tryBase(s);
            if (r.ok) {
                std::fprintf(stderr, "misterplexd: resolve ok via fallback server %s\n", s.c_str());
                return {r, s};
            }
        }
        return {resolved, selected};
    };

    auto doPlay = [&](const misterplex::PlayRequest& req) {
        // Owns :3005 for this play, including the Utility reload before playing()
        // is set. The listen hold from companion start is not cleared here.
        struct PlaySignalHold {
            explicit PlaySignalHold(misterplex::MediaPlayer&) {
                g_doPlayDepth.fetch_add(1, std::memory_order_acq_rel);
                g_signalHold.store(true, std::memory_order_release);
            }
            ~PlaySignalHold() {
                g_doPlayDepth.fetch_sub(1, std::memory_order_acq_rel);
            }
        } playSignalHold(player);
        uint64_t gen = req.dispatchGeneration;
        if (gen == 0) {
            std::lock_guard<std::mutex> handoff(playHandoffMu);
            gen = player.r1BeginPlayRequest([&] { return ++playGen; });
        }
        // R1 (review 06:57 D3, 07:30 C1, 08:17 (2)): no PLXH en=1 from this PLAY request to the
        // session's BEGIN. This doPlay owns its generation's hold while it runs (no expiry) and on
        // every exit releases it if it started no session and was not replaced by a seek's replay,
        // else leaves it to the session's enter or to the replay (r1_plxh.hpp PlayRequestHold).
        misterplex::r1::PlayRequestHold r1PlayHold(player.r1PlxhPolicy(), gen, [&, gen] {
            return gen == playGen.load() || !comp.wantPlay();
        });
        if (gen != playGen.load()) {
            std::fprintf(stderr, "misterplexd: PLAY superseded before resolve key=%s\n",
                         req.key.c_str());
            return;
        }
        int64_t off = req.offsetMs;
        const auto contentRes = contentResolutionForNextPlay();
        const auto displayRes = displayResolutionForNextPlay();
        const auto bankRes =
            misterplex::glassMax(contentRes.width, contentRes.height, player.liveGlass());
        // CLOSED-STOP: playMedia already planted wantPlay_ + fullScreenVideo.
        // Silent return after ACK/demux fail leaves Plex Web spinning.
        auto closedStopPlay = [&](const char* why, int64_t timeMs, int64_t durationMs) {
            const int bankW = player.decodeW();
            const int bankH = player.decodeH();
            const auto geom = misterplex::ddrFrameGeometryForPresentedSize(bankW, bankH);
            const auto layout = misterplex::makeDdrFrameLayout(
                geom, misterplex::ddrFramePhysBaseForGeometry(geom));
            const uint32_t plxjPhys = layout.doorbell_phys + 0x130u; // kPlxjOff, literal: see the LAB play-file log
            std::fprintf(stderr,
                         "misterplexd: CLOSED-STOP %s content=%s display=%s bank=%dx%d "
                         "doorbell=0x%08X PLXJ=0x%08X\n",
                         why, contentRes.label, displayRes.label, bankW, bankH,
                         layout.doorbell_phys, plxjPhys);
            comp.closedStop(timeMs, durationMs, player.fpgaH264Backend());
            pmsTimeline.endSession(timeMs, durationMs);
            std::lock_guard<std::mutex> lock(sessionMu);
            lastPlay = misterplex::PlayRequest{};
            lastBase.clear();
            seekReuse = misterplex::SeekReuseEntry{};
        };
        auto weakForPlay = weakForContentResolution(
            weak, contentRes, weakBitrateExplicit, weakQualityExplicit, weakH264ProfileExplicit);
        if (player.negotiatedHighCabac())
            weakForPlay.highCabac = true;
        // Library seekTo goes through doPlay (not MediaPlayer::seekMs). Mid-title
        // IDRs at the default 4Mbps 240p ladder measured 10489 > FPGA 8192 AU/RBSP.
        // Clamp bitrate/quality on any non-zero offset restart so PMS emits smaller AUs.
        const bool owenbSession =
            player.sessionBackend() == misterplex::VideoBackend::OwenbRing;
        if (player.fpgaH264Backend() && !owenbSession) {
            // 882b / true24 240p CAPS: 8KiB AU — default 4–8 Mbps IDRs measured
            // 9–10 KiB and CLOSED-STOP. Seek needs even smaller.
            // 480p ladder is 624x480 (validateWeakLadder floor 2000 kbps). Do not
            // crush it to the 240p 1500 ceiling or decision URL build fails empty.
            // e12+ CAPS may advertise larger AUs; still cap start bitrate for safety.
            // AU32 cores (CAPS 32KiB) tolerate larger all-IDR AUs than the old 8KiB ring.
            const bool tier480 =
                misterplex::fpgaAuSafeTier480(weakForPlay) ||
                contentRes.height >= 480 || contentRes.width >= 480 ||
                weakForPlay.profileName == "480i";
            const misterplex::FpgaAuSafeClamp auClamp =
                misterplex::fpgaAuSafeClamp(tier480, off);
            const int maxBr = auClamp.maxBitrateKbps;
            const int maxQ = auClamp.maxQuality;
            const int beforeBr = weakForPlay.maxVideoBitrateKbps;
            const int beforeQ = weakForPlay.videoQuality;
            if (weakForPlay.maxVideoBitrateKbps > maxBr)
                weakForPlay.maxVideoBitrateKbps = maxBr;
            if (weakForPlay.videoQuality > maxQ)
                weakForPlay.videoQuality = maxQ;
            if (beforeBr != weakForPlay.maxVideoBitrateKbps ||
                beforeQ != weakForPlay.videoQuality) {
                std::fprintf(stderr,
                             "misterplexd: FPGA AU-safe ladder offset_ms=%lld tier=%s "
                             "bitrate %d→%d quality %d→%d\n",
                             static_cast<long long>(off), tier480 ? "480" : "240", beforeBr,
                             weakForPlay.maxVideoBitrateKbps, beforeQ,
                             weakForPlay.videoQuality);
            }
        }
        std::fprintf(stderr,
                     "misterplexd: content=%s display=%s source=%s status_word=0x%04x "
                     "pms_videoResolution=%s bitrate=%d present=%dx%d glass=%s "
                     "doorbell=0x%08X PRESENT=fpga DDR path (SDRAM stick optional)\n",
                     contentRes.label, displayRes.label,
                     osdControl ? "OSD O[5:4]/O[15:14]" : "conf/--decode",
                     player.lastOsdWord(), weakForPlay.videoResolution.c_str(),
                     weakForPlay.maxVideoBitrateKbps, bankRes.width, bankRes.height,
                     misterplex::liveGlassLabel(player.liveGlass()),
                     misterplex::presentBankDoorbell(bankRes.width, bankRes.height));
        // MPX_BROWSE_V2: a browse ticket's ladder input (lead Q4: the helper's URL, or the
        // owning server's own transcode above ~8 Mbps), for its own item only (a queue step or
        // another key keeps the metadata Part). Never set for casts. The helper's
        // server-transcode relay carries its start in its path: a play at another start
        // re-fetches the ticket there, else the original Part (browseLadderInputForStart).
        if ((!req.ladderSrc.empty() || req.ladderInput == "server-transcode") &&
            req.ladderSrcFor == req.key) {
            const misterplex::BrowseLadderInput li = misterplex::browseLadderInputForStart(
                req.ladderSrc, req.ladderInput, req.ladderSrcOffsetMs, req.ladderTicketUrl,
                req.ratingKey, off, [](const std::string& url) {
                    return misterplex::browseHttpGet(url, misterplex::kMenuTimeoutMs,
                                                     misterplex::kTicketTimeoutMs, {});
                });
            if (li.refetched)
                std::fprintf(stderr, "misterplexd: browse ladder input: %s\n", li.note.c_str());
            if (!li.src.empty() || li.serverTranscode) {
                weakForPlay.ladderSrcOverride = li.src;
                weakForPlay.ladderServerTranscode = li.serverTranscode;
                std::fprintf(stderr, "misterplexd: browse ladder input for %s: %s\n",
                             req.key.c_str(),
                             !li.src.empty() ? "the helper's source"
                                             : "the owning server's transcode");
            }
        }
        if (!req.serverMachineId.empty()) {   // BUG1: this server's transcoder last emitted CAVLC above 60 % load
            int capKbps = 0;
            {
                std::lock_guard<std::mutex> lk(obTraitsMu);
                capKbps = obTraits.firstRequestKbps(req.serverMachineId, 0, static_cast<int64_t>(std::time(nullptr)));
            }
            if (capKbps > 0 && (weakForPlay.obMaxKbps <= 0 || capKbps < weakForPlay.obMaxKbps)) {
                weakForPlay.obMaxKbps = capKbps;
                std::fprintf(stderr, "misterplexd: OB PMS memory: %s last transcoded CAVLC above 60 %% load: "
                                     "PlexOB request capped at %d kbps\n", req.serverMachineId.c_str(), capKbps);
            }
        }
        if (req.pmsLowered) {
            weakForPlay = misterplex::lowerPmsLadder(weakForPlay);
            {   // BUG1-CAP: an OB "CAVLC at N kbps" retry asks the server for a rate the rewrite keeps up with
                int cap = 4000;
                for (const char* k : {"OB_CAVLC_MAX_KBPS", "MPX_OB_CAVLC_MAX_KBPS"})
                    if (const char* v = std::getenv(k); v && *v && std::atoi(v) > 0) {
                        cap = std::atoi(v);
                        break;
                    }
                const int before = weakForPlay.maxVideoBitrateKbps;
                weakForPlay = misterplex::owenbCavlcRetryLadder(weakForPlay, req.gateDetail, cap);
                if (weakForPlay.maxVideoBitrateKbps != before)
                    std::fprintf(stderr, "misterplexd: OB CAVLC retry: maxVideoBitrate %d -> %d (OB_CAVLC_MAX_KBPS=%d)\n",
                                 before, weakForPlay.maxVideoBitrateKbps, cap);
            }
            std::fprintf(stderr, "misterplexd: lowered PMS retry key=%s bitrate=%d quality=%d (strict Extra)\n",
                         req.key.c_str(), weakForPlay.maxVideoBitrateKbps, weakForPlay.videoQuality);
            if (!req.gateDetail.empty() && req.gateDetail.find("pic_width_in_mbs") != std::string::npos &&
                misterplex::CoreGate::process().pairedRaster() == "240p") {
                misterplex::WeakLadder w240;
                if (misterplex::applyPlexTranscodeProfile("240p", w240)) {
                    const int br = weakForPlay.maxVideoBitrateKbps;
                    const int q = weakForPlay.videoQuality;
                    weakForPlay.videoResolution = w240.videoResolution;
                    weakForPlay.profileName = w240.profileName;
                    weakForPlay.maxVideoBitrateKbps = br;
                    weakForPlay.videoQuality = q;
                    std::fprintf(stderr,
                                 "misterplexd: R16 240p R6p retry videoResolution=%s profile=%s\n",
                                 weakForPlay.videoResolution.c_str(), weakForPlay.profileName.c_str());
                }
            }
        }
        misterplex::ResolveResult resolved;
        std::string base;
        bool seekReused = false;
        // 176 L4-L7: the worker-exit clear never logged. Tear down here, before
        // this play reuses the session that just refused a High AU.
        const bool toreDown = player.takeGateRefuse();
        if (toreDown)
            player.teardownAfterGateRefuse();
        {
            std::string why;
            std::lock_guard<std::mutex> lock(sessionMu);
            // Prod line: one core per daemon, so the core key is constant (0).
            // A gate refuse must not reuse the session that delivered the High AU.
            if (!toreDown)
                seekReused = misterplex::seekReuseFor(seekReuse, req, weakForPlay, 0, off, &resolved,
                                                      &base, &why);
            if (req.seekReplay)
                std::fprintf(stderr, "misterplexd: seek %s key=%s off=%lld (%s)\n",
                             seekReused ? "reuses the committed PMS session (no decision)"
                                        : "re-resolves",
                             req.key.c_str(), static_cast<long long>(off), why.c_str());
        }
        if (!seekReused) {
            // A later seek/play bumps playGen: the decision is then never issued (one per coalesced seek).
            misterplex::setResolveAbortCheck([&playGen, gen] { return gen != playGen.load(); });
            auto rb = resolveAgainstServers(req, defaultPms, off, weakForPlay, contentRes.width,
                                            contentRes.height);
            misterplex::setResolveAbortCheck({});
            resolved = std::move(rb.first);
            base = std::move(rb.second);
            // NO-REQUIRED-PROFILE: the decision did not fit the loaded core: one lowered request, then the SPS
            // gate decides (docs/plans/NO-REQUIRED-PROFILE.md).
            const std::string mismatch = misterplex::takeDecisionMismatch();
            if (!mismatch.empty() && !weakForPlay.pmsLowered && !resolved.viaLadderProxy &&
                gen == playGen.load()) {
                weakForPlay = misterplex::lowerPmsLadder(weakForPlay);
                std::fprintf(stderr,
                             "misterplexd: decision mismatch (%s): one lowered PMS retry key=%s bitrate=%d\n",
                             mismatch.c_str(), req.key.c_str(), weakForPlay.maxVideoBitrateKbps);
                misterplex::setResolveAbortCheck([&playGen, gen] { return gen != playGen.load(); });
                auto rl = resolveAgainstServers(req, defaultPms, off, weakForPlay, contentRes.width,
                                                contentRes.height);
                misterplex::setResolveAbortCheck({});
                (void)misterplex::takeDecisionMismatch();
                resolved = std::move(rl.first);
                base = std::move(rl.second);
            }
        }

        if (gen != playGen.load()) {
            std::fprintf(stderr, "misterplexd: PLAY superseded during resolve key=%s\n",
                         req.key.c_str());
            return;
        }

        if (!resolved.ok) {
            if (player.fpgaH264Backend()) {
                std::lock_guard<std::mutex> failedHandoff(playHandoffMu);
                if (gen != playGen.load() || !comp.acceptsPlayRequest(req))
                    return;
                misterplex::retirePlayback(player, pmsTimeline);
                if (gen != playGen.load())
                    return;
                std::fprintf(stderr, "misterplexd: FPGA resolve failed: %s\n",
                             resolved.detail.c_str());
                closedStopPlay("FPGA resolve failed; no fallback", off, resolved.durationMs);
                return;
            }
            std::fprintf(stderr, "misterplexd: resolve failed: %s — test pattern\n",
                         resolved.detail.c_str());
            resolved.playable = "testsrc";
            resolved.ok = true;
            resolved.durationMs = 120000;
            resolved.sourceFpsHint = 30; // testsrc default
            resolved.fpsNum = 30;
            resolved.fpsDen = 1;
            resolved.sourceAspect = {4, 3, true};
        } else {
            std::fprintf(stderr, "misterplexd: resolved %s title=%s dur=%lld transcode=%d base=%s\n",
                         resolved.detail.c_str(), resolved.title.c_str(),
                         static_cast<long long>(resolved.durationMs),
                         resolved.transcoded ? 1 : 0, base.c_str());
        }
        {   // BUG1: the entropy the OB play reports is this server's transcoder's only when PMS transcodes
            std::lock_guard<std::mutex> lk(obTraitsMu);
            obTraitsKey = resolved.transcoded ? req.serverMachineId : std::string();
        }
        if (player.fpgaH264Backend() && resolved.ok &&
            (resolved.decisionWidth <= 0 || resolved.decisionHeight <= 0)) {
            const misterplex::DecisionVideo dv = misterplex::lastDecisionVideo();
            if (dv.width > 0 && dv.height > 0) {
                resolved.decisionWidth = dv.width;
                resolved.decisionHeight = dv.height;
                std::fprintf(stderr,
                             "misterplexd: play decision geometry pinned from resolve thread %dx%d\n",
                             dv.width, dv.height);
            }
        }

        int resolvedFpsNum = resolved.fpsNum;
        int resolvedFpsDen = resolved.fpsDen;
        std::unique_lock<std::mutex> handoff;
        // Wire SOURCE_FPS / MATCH_SOURCE_HZ into play path (software Content FPS hint).
        const int effective =
            misterplex::applySourceFpsConf(sourceFpsConf, resolved.sourceFpsHint);
        if (effective > 0) {
            std::fprintf(stderr,
                         "misterplexd: Content FPS hint=%d (SOURCE_FPS=%s pms_vfr=%s "
                         "frameRate=%s resolved=%d) — exact pacing uses resolved rate; "
                         "switchres TODO\n",
                         effective, sourceFpsConf.c_str(),
                         resolved.videoFrameRate.empty() ? "-" : resolved.videoFrameRate.c_str(),
                         resolved.frameRate.empty() ? "-" : resolved.frameRate.c_str(),
                         resolved.sourceFpsHint);
        } else {
            std::fprintf(stderr,
                         "misterplexd: Content FPS hint unknown (SOURCE_FPS=%s)\n",
                         sourceFpsConf.c_str());
        }
        if (confTruthy(matchSourceHz) || matchSourceHz == "on" || matchSourceHz == "1") {
            std::fprintf(stderr,
                         "misterplexd: match-source-Hz ON target≈%dHz — switchres not "
                         "wired (cadence path active; see docs/match-source-hz.md)\n",
                         effective > 0 ? effective : 0);
        }

        // Exact rational rate for A/V pacing. This is deliberately NOT the bucketed
        // hint above: PMS reports Media@videoFrameRate="24p" for 23.976 content, and
        // pacing that at 24 costs ~1 ms/s of lipsync drift.
        if (!player.fpgaH264Backend()) {
            misterplex::applyContentFpsConf(loadConf(confPath, "AV_CONTENT_FPS"),
                                            resolvedFpsNum, resolvedFpsDen);
            if (resolvedFpsNum > 0 && resolvedFpsDen > 0)
                misterplex::capExactFpsToMaxHz(resolvedFpsNum, resolvedFpsDen, 30.0);
        }
        std::fprintf(stderr,
                     "misterplexd: content fps exact=%d/%d (pms frameRate=%s vfr=%s "
                     "match_cap=30Hz)\n",
                     resolvedFpsNum, resolvedFpsDen,
                     resolved.frameRate.empty() ? "-" : resolved.frameRate.c_str(),
                     resolved.videoFrameRate.empty() ? "-" : resolved.videoFrameRate.c_str());
        if (!resolved.sourceAspect.valid && !player.fpgaH264Backend()) {
            resolved.sourceAspect =
                player.probeSourceAspect(resolved.playable, resolved.httpHeaders);
            if (resolved.sourceAspect.valid) {
                std::fprintf(stderr,
                             "misterplexd: source aspect probed from stream=%u:%u\n",
                             resolved.sourceAspect.x, resolved.sourceAspect.y);
            }
        }

        // Plex Web may send only containerKey=/playQueues/N. A ratingKey is not a
        // playQueueItemID; inventing one makes the controller index a nonexistent
        // queue row and discard otherwise advancing timeline polls.
        std::string resolvedPlayQueueItemId = req.playQueueItemId;
        std::string resolvedPlayQueueId = req.playQueueId;
        std::string resolvedPlayQueueVersion = req.playQueueVersion;
        std::string resolvedContainerKey = req.containerKey;
        if (resolvedPlayQueueItemId.empty()) {
            std::string queueRef = req.containerKey;
            if (queueRef.empty() && !req.playQueueId.empty())
                queueRef = "/playQueues/" + req.playQueueId;
            std::string queueToken = req.token;
            if (queueToken.empty() && req.address.empty())
                queueToken = confToken;
            if (!queueRef.empty() && queueRef.find("/playQueues/") != std::string::npos) {
                const auto queue =
                    misterplex::fetchPlayQueue(queueRef, base, queueToken, req.key, {});
                if (queue.ok && queue.currentIndex >= 0 &&
                    static_cast<size_t>(queue.currentIndex) < queue.items.size()) {
                    const auto& item = queue.items[static_cast<size_t>(queue.currentIndex)];
                    if (misterplex::firstResolveBinds(item, req.key, resolved.ratingKey) &&
                        !item.playQueueItemId.empty()) {
                        resolvedPlayQueueItemId = item.playQueueItemId;
                        if (!queue.playQueueId.empty())
                            resolvedPlayQueueId = queue.playQueueId;
                        if (!queue.playQueueVersion.empty())
                            resolvedPlayQueueVersion = queue.playQueueVersion;
                        if (!queue.containerKey.empty())
                            resolvedContainerKey = queue.containerKey + "?own=1";
                        // #25 b1: the queue detail names the pass that hit (index=N by=id|key|rk).
                        std::fprintf(stderr,
                                     "misterplexd: play queue bound id=%s item=%s version=%s (%s)\n",
                                     resolvedPlayQueueId.c_str(),
                                     resolvedPlayQueueItemId.c_str(),
                                     resolvedPlayQueueVersion.empty()
                                         ? "-"
                                         : resolvedPlayQueueVersion.c_str(),
                                     queue.detail.c_str());
                    }
                } else {
                    std::fprintf(stderr,
                                 "misterplexd: play queue identity unavailable: %s\n",
                                 queue.detail.c_str());
                }
            }
        }

        handoff = std::unique_lock<std::mutex>(playHandoffMu);
        if (gen != playGen.load() || !comp.acceptsPlayRequest(req)) {
            std::fprintf(stderr,
                         "misterplexd: PLAY superseded before aspect commit key=%s\n",
                         req.key.c_str());
            return;
        }
        misterplex::retirePlayback(player, pmsTimeline);
        FpgaWorkerHandoff fpgaWorkers(player);
        // FPGA fabric CAPS follow coded content (320x240 on 882b). Glass bank
        // (true480 640x480) is present-store only — feeding bank as DECODE made
        // resolve/CAPS treat the ladder as 624x480 and stall after a few AUs.
        if (player.fpgaH264Backend()) {
            player.setDecodeSize(contentRes.width, contentRes.height);
            std::fprintf(stderr,
                         "misterplexd: FPGA DECODE=%dx%d (content); present bank %dx%d glass=%s\n",
                         contentRes.width, contentRes.height, bankRes.width, bankRes.height,
                         misterplex::liveGlassLabel(player.liveGlass()));
        } else {
            player.setDecodeSize(bankRes.width, bankRes.height);
        }
        player.setContentFpsRational(resolvedFpsNum, resolvedFpsDen);
        player.setPlaybackTitle(resolved.title);
        player.setSourceTranscoded(resolved.transcoded);
        bool sourceAspectPublished = player.setSourceAspect(resolved.sourceAspect);
        // RETRY-GLASS-CAP: display≠content ACK fail retries glassMax(content),
        // never raw 1280x720 (true480 L4 poke 0x3047F000) or a shrink on L4.
        // 720/720 still skips (display==content). Keep the retry — do not drop
        // the rtl_invariants display≠content needle.
        if (!player.fpgaH264Backend() && !sourceAspectPublished &&
            (displayRes.width != contentRes.width || displayRes.height != contentRes.height)) {
            const auto retryBank = misterplex::glassMax(
                contentRes.width, contentRes.height, player.liveGlass());
            std::fprintf(
                stderr,
                "misterplexd: source aspect display layout ACK failed; retrying "
                "glass-capped content layout %dx%d (content %dx%d glass=%s)\n",
                retryBank.width, retryBank.height, contentRes.width, contentRes.height,
                misterplex::liveGlassLabel(player.liveGlass()));
            player.setDecodeSize(retryBank.width, retryBank.height);
            sourceAspectPublished = player.setSourceAspect(resolved.sourceAspect);
        }
        if (!sourceAspectPublished) {
            std::fprintf(stderr,
                         "misterplexd: PLAY rejected: source aspect unknown or FPGA ACK "
                         "did not match\n");
            closedStopPlay("PLAY rejected: source aspect unknown or FPGA ACK did not match",
                           off, resolved.durationMs);
            return;
        }
        player.setSourceMediaSize(resolved.mediaWidth, resolved.mediaHeight);
        player.setSourceInterlaced(resolved.mediaInterlaced);
        player.setSourceHasAudio(resolved.hasAudio);
        if (resolved.mediaWidth > 0 && resolved.mediaHeight > 0) {
            std::fprintf(stderr,
                         "misterplexd: pms source media=%dx%d decode=%dx%d scale=%s "
                         "hasAudio=%d\n",
                         resolved.mediaWidth, resolved.mediaHeight, player.decodeW(),
                         player.decodeH(),
                         (resolved.mediaWidth == player.decodeW() &&
                          resolved.mediaHeight == player.decodeH())
                             ? "identity_match_bank"
                             : "scale_or_pad",
                         resolved.hasAudio ? 1 : 0);
        } else {
            std::fprintf(stderr, "misterplexd: pms hasAudio=%d\n",
                         resolved.hasAudio ? 1 : 0);
        }

        if (!req.offsetPresent && resolved.viewOffsetMs > 0) {
            // PMS continue-watching offset when cast omitted offset=. playMedia already
            // planted scrubTarget at 0; demux will start at viewOffset. Re-plant so the
            // companion hold matches the real start (avoids far-ahead freeze at 0:00).
            off = resolved.viewOffsetMs;
            std::fprintf(stderr,
                         "misterplexd: applying PMS viewOffsetMs=%lld (cast offset absent)\n",
                         static_cast<long long>(off));
        }

        misterplex::PlayRequest bound = req;
        if (bound.ratingKey.empty())
            bound.ratingKey = resolved.ratingKey;
        if (bound.playQueueItemId.empty())
            bound.playQueueItemId = resolvedPlayQueueItemId;
        if (bound.playQueueId.empty())
            bound.playQueueId = resolvedPlayQueueId;
        if (bound.playQueueVersion.empty())
            bound.playQueueVersion = resolvedPlayQueueVersion;
        if (bound.containerKey.empty())
            bound.containerKey = resolvedContainerKey;
        if (bound.address.empty() && !base.empty()) {
            auto hostport = base;
            auto p = hostport.find("://");
            if (p != std::string::npos)
                hostport = hostport.substr(p + 3);
            auto slash = hostport.find('/');
            if (slash != std::string::npos)
                hostport = hostport.substr(0, slash);
            auto colon = hostport.rfind(':');
            if (colon != std::string::npos) {
                bound.address = hostport.substr(0, colon);
                bound.port = hostport.substr(colon + 1);
            } else {
                bound.address = hostport;
                bound.port = "32400";
            }
            if (base.rfind("https", 0) == 0)
                bound.protocol = "https";
            else
                bound.protocol = "http";
        }
        if (bound.serverMachineId.empty())
            bound.serverMachineId = "plex-server";

        bound.letterbox = false;
        bound.active_height = 0;
        bound.pic_height = 0;
        bound.crop_height = 0;
        bound.cropLeft = 0;
        bound.cropRight = 0;
        bound.cropTop = 0;
        bound.cropBottom = 0;
        bound.is480i = false;
        if (contentRes.label && std::strcmp(contentRes.label, "240p") == 0) {
            misterplex::apply240pLetterbox(bound, resolved.mediaWidth, resolved.mediaHeight);
            misterplex::apply240pActiveHeight(bound, resolved.mediaWidth, resolved.mediaHeight,
                                              bound.cropLeft, bound.cropRight, bound.cropTop,
                                              bound.cropBottom);
            std::fprintf(stderr,
                         "misterplexd: 240p letterbox=%d active_height=%d pic_height=%d coded=%dx%d\n",
                         bound.letterbox, bound.active_height, bound.pic_height,
                         resolved.mediaWidth, resolved.mediaHeight);
        } else if (contentRes.label && std::strcmp(contentRes.label, "480p") == 0) {
            misterplex::apply480pLetterbox(bound, resolved.mediaWidth, resolved.mediaHeight);
            std::fprintf(stderr,
                         "misterplexd: 480p letterbox=%d active_height=%d coded=%dx%d\n",
                         bound.letterbox, bound.active_height, resolved.mediaWidth,
                         resolved.mediaHeight);
        } else if (contentRes.label && std::strcmp(contentRes.label, "480i") == 0) {
            misterplex::apply480iLetterbox(bound, resolved.mediaWidth, resolved.mediaHeight);
            std::fprintf(stderr,
                         "misterplexd: 480i letterbox=%d active_height=%d coded=%dx%d\n",
                         bound.letterbox, bound.active_height, resolved.mediaWidth,
                         resolved.mediaHeight);
        }

        if (gen != playGen.load() || !comp.wantPlay()) {
            std::fprintf(stderr, "misterplexd: PLAY superseded before bind key=%s\n",
                         bound.key.c_str());
            return;
        }

        if (!comp.bindMedia(bound, resolved.durationMs)) {
            // Stop won the race against async playMedia resolve — do not restart player.
            // Do not write lastPlay here: stop may have cleared it; a failed bind must
            // not resurrect the prior/new queue for a post-stop skipNext race.
            std::fprintf(stderr, "misterplexd: PLAY aborted (stopped during resolve) key=%s\n",
                         bound.key.c_str());
            return;
        }

        // Honor scrubber seeks/steps that landed while resolve was in flight.
        // playMedia seeded timeMs_=req.offsetMs; if the user moved the timeline,
        // start there instead of rewinding to the original cast offset.
        // A scrub that lands after this read (up to player.play below) is not folded into this
        // URL (pre-existing, dbcc42fa). The seek bumps playGen under the companion lock
        // (TransportGenerations::accept from acceptSeekLocked) and the seek handler
        // (playback_controls.hpp) replays at the scrub target, which the replay reads back from
        // the companion plant; this doPlay stops at a later generation check or, past the last
        // one, is replaced by that replay. The plant re-seed below is guarded under the same lock
        // so this stale start cannot overwrite the seek's plant. Cost: one more resolve/restart.
        int64_t startAt = off;
        const int64_t scrubT = comp.timelineTimeMs();
        if (scrubT != req.offsetMs)
            startAt = scrubT;
        // [0, duration]; an unknown duration (0) caps at kMaxUnknownDurationStartMs (one week).
        startAt = misterplex::transcode_offset::clampStartMs(startAt, resolved.durationMs);

        // The playable carries the start it was resolved for. A continue-watching start (PMS
        // viewOffset, cast offset absent) or a scrub that landed during resolve moves startAt:
        // bake it into the URL the way a seek restart does. Otherwise the FPGA demuxer is asked
        // to av_seek_frame a live transcode / ladder TS while the timeline reports startAt.
        {
            const std::string retargeted =
                misterplex::transcode_offset::retargetStartUrl(resolved.playable, startAt);
            if (retargeted != resolved.playable) {
                std::fprintf(stderr,
                             "misterplexd: start %lld (cast offset %lld) differs from the "
                             "resolved URL start; URL start retargeted\n",
                             static_cast<long long>(startAt),
                             static_cast<long long>(req.offsetMs));
                resolved.playable = retargeted;
            }
        }

        // Stop / newer playMedia may still race after bindMedia: re-check before
        // setState/player.play so we never restart demux on a stopped session.
        if (gen != playGen.load() || !comp.wantPlay()) {
            std::fprintf(stderr, "misterplexd: PLAY aborted after bind key=%s\n",
                         bound.key.c_str());
            return;
        }

        // Re-base companion plant to the demux start (viewOffset / in-flight seek).
        // setState(buffering) alone cannot move time while an older plant holds.
        // (0abee0b6 — stop Web scrubber freeze at 0:00 when demux is ahead of plant)
        // Both writes run only while this doPlay is current, checked under the companion lock:
        // a seek accepted after the check above must keep its plant (it is the replay's start).
        const auto stillCurrent = [&] { return gen == playGen.load(); };
        comp.seedPlaybackPosition(startAt, resolved.durationMs, stillCurrent);

        // Ensure timeline immediately reports duration + time for scrubber (seekRange).
        comp.setState("buffering", startAt, resolved.durationMs, false, stillCurrent);

        if (gen != playGen.load() || !comp.wantPlay()) {
            std::fprintf(stderr, "misterplexd: PLAY superseded before player.play key=%s\n",
                         bound.key.c_str());
            return;
        }

        // Commit session context only when we are about to start demux. Writing
        // lastPlay earlier can resurrect a queue bind if stop cleared it mid-flight.
        // setPlay already planted a provisional lastPlay for skip-during-resolve.
        // Final wantPlay/playGen gate under the same critical section as lastPlay
        // so stop cannot clear then get a zombie lastPlay + player.play.
        {
            std::lock_guard<std::mutex> lock(sessionMu);
            if (gen != playGen.load() || !comp.wantPlay()) {
                std::fprintf(stderr, "misterplexd: PLAY superseded before demux key=%s\n",
                             bound.key.c_str());
                return;
            }
            lastPlay = bound;
            lastBase = base;
            seekReuse = misterplex::makeSeekReuseEntry(bound, weakForPlay, 0, resolved, base);
            // SPS-gate L3: the exact request this generation committed (the gate fallback re-plays only this one).
            committedPlay = bound;
            committedPlayGen = gen;
            if (!bound.token.empty())
                lastToken = bound.token;
            else if (req.address.empty() && !confToken.empty())
                lastToken = confToken;
        }

        // Timeline must target the server that issued playMedia, with THAT request's
        // credentials. Mixing cast host (e.g. *.plex.direct) with conf PLEX_TOKEN from
        // a different PMS yields HTTP 401 while the old !body.empty() sink still looked ok
        // (docs/evidence/timeline-stuck-20260730T173000Z / eeae7943).
        std::string timelineToken;
        const char* tokenSrc = "none";
        if (!bound.token.empty()) {
            timelineToken = bound.token;
            tokenSrc = "cast";
        } else if (req.address.empty() && !confToken.empty()) {
            timelineToken = confToken;
            tokenSrc = "conf";
        }
        misterplex::PmsTimelineSession timelineSession;
        timelineSession.baseUrl = base;
        timelineSession.token = timelineToken;
        timelineSession.key = bound.key;
        timelineSession.ratingKey = bound.ratingKey;
        timelineSession.playQueueItemId = bound.playQueueItemId;
        timelineSession.containerKey = bound.containerKey;
        timelineSession.clientIdentifier = machineId;
        timelineSession.product = "MiSTerPlex";
        timelineSession.clientProfileName = !resolved.clientProfileName.empty()
                                                ? resolved.clientProfileName
                                                : misterplex::defaultRequestProfileName();
        timelineSession.version = "0.4.1";
        timelineSession.deviceName = name;
        std::fprintf(stderr,
                     "misterplexd: pms timeline session base=%s token_src=%s ratingKey=%s\n",
                     base.c_str(), tokenSrc, timelineSession.ratingKey.c_str());
        pmsTimeline.beginSession(timelineSession, startAt, resolved.durationMs);

        activePlayGen.store(gen);
        // SPS-gate L3: this play's gate context. helperStream: resolve built the ladder helper's /ladder.ts (e.g.
        // ladder480 on a 480p conf), so a refusal at AU0 is the helper's own (R8), never a fallback to the same
        // ladder.
        misterplex::gate::GatePlayContext gateCtx;
        gateCtx.fallbackUsed = req.fallbackUsed;
        gateCtx.helperStream = resolved.viaLadderProxy;
        gateCtx.helperConfigured = misterplex::ladderProxyConfigured();
        gateCtx.pmsRetryAvailable = !req.pmsLowered && !weakForPlay.pmsLowered && !req.fallbackUsed &&
                                    !resolved.viaLadderProxy && resolved.transcoded &&
                                    resolved.playable.find("/transcode/universal/") != std::string::npos;
        if (req.fallbackUsed || req.forceLadder)
            std::fprintf(stderr, "misterplexd: GATE fallback play key=%s off=%lld via_ladder=%d detail=%s\n",
                         bound.key.c_str(), static_cast<long long>(startAt), resolved.viaLadderProxy ? 1 : 0,
                         resolved.detail.c_str());
        if (player.fpgaH264Backend() &&
            misterplex::legacyMenuReloadApplies(player.sessionBackend())) {
            uint32_t menuTarget = misterplex::menuReloadTargetForPlay(
                contentRes.label, resolved.mediaWidth, resolved.mediaHeight,
                resolved.mediaInterlaced, weakForPlay.profileName,
                weakForPlay.videoResolution, resolved.mediaProgressive && !resolved.mediaInterlaced);
            int decisionW = resolved.decisionWidth;
            int decisionH = resolved.decisionHeight;
            if (decisionW <= 0 || decisionH <= 0) {
                const misterplex::DecisionVideo dv = misterplex::lastDecisionVideo();
                decisionW = dv.width;
                decisionH = dv.height;
            }
            menuTarget = misterplex::menuReloadUtilityTarget(
                misterplex::menuTargetAfterDecision(menuTarget, decisionW, decisionH));
            const uint32_t loadedBefore = misterplex::CoreGate::process().buildId();
            menuTarget = misterplex::menuTargetAfterLoweredRetry(
                menuTarget, loadedBefore, decisionW, decisionH, req.pmsLowered);
            const uint32_t loadedBuild = misterplex::CoreGate::process().buildId();
            // 176 L1: a 480p decision can leave menuTarget at 0 while prod
            // 0x487A1BD1 is loaded. That is not the Utility target.
            if (loadedBuild == misterplex::kSelectedCoreId480p &&
                (menuTarget == 0 || menuTarget == misterplex::kSelectedCoreId480p))
                menuTarget = misterplex::kUtilityCoreId480p;
            // The bank follows the final target. A 240p leg leaves decode at
            // 320x240, and the next 480p/480i play was still presenting that bank.
            const int bankW = misterplex::presentBankWidthForMenuTarget(menuTarget);
            const int bankH = misterplex::presentBankHeightForMenuTarget(menuTarget);
            if (bankW > 0 && bankH > 0) {
                player.setDecodeSize(bankW, bankH);
                std::fprintf(stderr,
                             "misterplexd: session epoch present bank %dx%d target=0x%08X\n",
                             bankW, bankH, menuTarget);
            }
            std::fprintf(stderr,
                         "misterplexd: menu reload plan content=%s decision=%dx%d "
                         "(resolved=%dx%d) target=0x%08X loaded=0x%08X\n",
                         contentRes.label, decisionW, decisionH, resolved.decisionWidth,
                         resolved.decisionHeight, menuTarget, loadedBuild);
            if (menuTarget) {
                const uint32_t paired = misterplex::CoreGate::process().buildId();
                if (misterplex::menuTargetSatisfied(menuTarget, paired)) {
                    const char* coreLine = misterplex::menuReloadCoreLine(menuTarget);
                    std::fprintf(stderr,
                                 "media: menu reload paired 0x%08X loaded 0x%08X %s "
                                 "already-satisfied\n",
                                 menuTarget, paired, coreLine ? coreLine : "load_core");
                    std::fprintf(stderr,
                                 "misterplexd: core gate open: 'Plex' build 0x%08X "
                                 "(session epoch; already loaded)\n",
                                 menuTarget);
                } else if (!player.tryMenuReloadForTargetBuild(menuTarget)) {
                    std::fprintf(stderr,
                                 "misterplexd: menu reload incomplete target=0x%08X (gate "
                                 "build 0x%08X open=%d name=%s)\n",
                                 menuTarget, misterplex::CoreGate::process().buildId(),
                                 misterplex::CoreGate::process().open() ? 1 : 0,
                                 misterplex::CoreGate::process().name().c_str());
                    closedStopPlay("menu reload incomplete before PLAY", startAt,
                                   resolved.durationMs);
                    return;
                }
            }
            // After the Utility load, a Baseline core needs a session whose
            // resolution matches that core. 174 L3 stayed profile_idc 100 at
            // 320x240. 174 L6 kept the 480i core but played the 320x240 URL.
            const char* coreRes = misterplex::baselineCorePlayResolution(menuTarget);
            const bool coreResMismatch =
                coreRes && !misterplex::playUrlHasVideoResolution(resolved.playable, coreRes);
            if (menuTarget == misterplex::kSelectedCoreId240p || coreResMismatch) {
                auto coreWeak = weakForPlay;
                coreWeak.videoResolution = coreRes ? coreRes : "320x240";
                coreWeak.h264Profile = "baseline";
                coreWeak.h264ProfileParam = "baseline";
                coreWeak.highCabac = false;
                misterplex::setResolveAbortCheck([&playGen, gen] { return gen != playGen.load(); });
                auto again = resolveAgainstServers(req, base.empty() ? defaultPms : base, off, coreWeak,
                                                   contentRes.width, contentRes.height);
                if (!again.first.ok || again.first.playable.empty()) {
                    std::fprintf(stderr,
                                 "misterplexd: 240p core re-resolve failed to pin baseline\n");
                    closedStopPlay("240p re-resolve did not return a baseline playable", startAt,
                                   resolved.durationMs);
                    return;
                }
                resolved = again.first;
                if (!again.second.empty())
                    base = again.second;
                std::fprintf(stderr,
                             "misterplexd: baseline core re-resolve videoResolution=%s "
                             "videoProfile=baseline target=0x%08X\n",
                             coreWeak.videoResolution.c_str(), menuTarget);
            }
        }
        // 176 L1 played on prod and HDMI stayed the chevron. Reload before the
        // bitstream even when the plan block above did not run.
        if (misterplex::legacyMenuReloadApplies(player.sessionBackend()) &&
            misterplex::CoreGate::process().buildId() == misterplex::kSelectedCoreId480p) {
            std::fprintf(stderr,
                         "misterplexd: matrix-start reload prod 0x487A1BD1 -> Plex_480p\n");
            if (!player.tryMenuReloadForTargetBuild(misterplex::kUtilityCoreId480p))
                std::fprintf(stderr,
                             "misterplexd: matrix-start reload failed gate=0x%08X\n",
                             misterplex::CoreGate::process().buildId());
        }
        std::fprintf(stderr, "misterplexd: PLAY %s off=%lld dur=%lld\n",
                     misterplex::redactUrlTokens(resolved.playable).c_str(),
                     static_cast<long long>(startAt), static_cast<long long>(resolved.durationMs));
        if (!player.play(resolved.playable, startAt, resolved.httpHeaders,
                         resolved.durationMs, gen, gateCtx)) {
            std::fprintf(stderr, "misterplexd: PLAY failed to start demux key=%s\n",
                         bound.key.c_str());
            closedStopPlay("PLAY failed to start demux", startAt, resolved.durationMs);
            return;
        }
        r1PlayHold.started();   // the session of gen takes the hold over at its enter
        fpgaWorkers.completePlayback();
    };

    // GCAST (P1/P2): AirPlay / YouTube cast plays through the SessionArbiter. With GAIRPLAY and
    // GYT off (the default) nothing below runs and the Plex path is unchanged.
    misterplex::gcast::SessionArbiter gcastArbiter;
    const bool gcastAirplay = misterplex::envFlagWanted("GAIRPLAY");
    const bool gcastYt = misterplex::envFlagWanted("GYT");
    const bool gcastOn = gcastAirplay || gcastYt;
    bool gcastApUp = false;
    std::atomic<bool> gcastYtUp{false};   // P4: YouTube ready flips from the Lounge thread and the main loop
    std::atomic<bool> gcastApReady{false};   // Ready-card AirPlay: helper up and /probe self-test passed
    std::atomic<uint64_t> castGen{0};
    std::atomic<uint64_t> castToken{0};
    auto castPlay = [&](misterplex::gcast::Owner owner, const std::string& url, double startS,
                        const std::string& title, int64_t durHintMs, int64_t resumeMs,
                        bool isSeek = false) -> bool {
        using namespace misterplex::gcast;
        uint64_t gen;
        {
            std::lock_guard<std::mutex> handoff(playHandoffMu);
            gen = player.r1BeginPlayRequest([&] { return ++playGen; });   // in-flight Plex doPlay aborts
        }
        castGen.store(gen);
        const uint64_t tok = gcastArbiter.claim(owner);   // retires Plex (closedStop) or the other caster
        castToken.store(tok);
        // Gate 2b: a skip is a new play while the previous stream is still on glass.
        // Stop it before yt-dlp. playGen already moved and activePlayGen still names
        // the old play, so this stop's progress is not applied to the new Lounge id.
        // A failed resolve then leaves the chevron, matching the Stopped state the sink reports.
        {
            const auto tear = castGlassAction(GlassStep::NewPlay, player.playing(), true, owner,
                                              gcastArbiter.owner(), true, !isSeek);
            if (tear.stopMedia) {
                std::lock_guard<std::mutex> handoff(playHandoffMu);
                if (gen == playGen.load())
                    misterplex::retirePlayback(player, pmsTimeline);
            }
        }
        misterplex::r1::PlayRequestHold r1PlayHold(player.r1PlxhPolicy(), gen,
                                                   [&, gen] { return gen == playGen.load(); });
        const auto contentRes = contentResolutionForNextPlay();
        const auto bankRes = misterplex::glassMax(contentRes.width, contentRes.height, player.liveGlass());
        const auto weakForPlay = weakForContentResolution(weak, contentRes, weakBitrateExplicit,
                                                          weakQualityExplicit, weakH264ProfileExplicit);
        const auto session = misterplex::sessionVideoBackend(misterplex::configuredVideoBackend(),
                                                             misterplex::CoreGate::process().ownsIdleInFabric());
        const bool fpga = session == misterplex::VideoBackend::FpgaH264;
        const auto ladder = misterplex::ladderForPlaySession(session, weakForPlay, false);
        int lw = 0, lh = 0;
        if (!parseWxH(ladder.videoResolution, lw, lh)) {
            lw = contentRes.width;
            lh = contentRes.height;
        }
        if (fpga && lh > 240 && lw > misterplex::fpga480pCodedWidthCeiling())
            lw = misterplex::fpga480pCodedWidthCeiling();   // 39-MB cores: 624x480 (SPS gate pic_width_in_mbs)
        const int br = ladder.maxVideoBitrateKbps > 0 ? std::min(ladder.maxVideoBitrateKbps, 4000) : 3000;
        const int64_t startMs = startS > 0 ? static_cast<int64_t>(startS * 1000.0) : 0;
        // GCAST_XCODE optional: healthy remote preferred; else local standalone yt-dlp helper.
        static std::string xcodeHealthCache;
        static time_t xcodeHealthAt = 0;
        auto xcodeHealthy = [&]() -> bool {
            const std::string remote = helperBaseFromEnv();
            if (remote.empty()) return false;
            const time_t now = time(nullptr);
            if (xcodeHealthCache == remote && now - xcodeHealthAt < 30)
                return true;
            if (xcodeHealthCache == "!" + remote && now - xcodeHealthAt < 15)
                return false;
            const std::string body = misterplex::plexHttpGet(remote + "/health", 2);
            const bool ok = body.find("ok") != std::string::npos;
            xcodeHealthCache = ok ? remote : ("!" + remote);
            xcodeHealthAt = now;
            if (!ok)
                std::fprintf(stderr, "misterplexd: GCAST_XCODE down; trying standalone\n");
            return ok;
        };
        const bool remoteOk = xcodeHealthy();
        const bool localOk = remoteOk ? false : misterplex::gcast::cachedLoopbackHelperHealth();
        const std::string xcode = effectiveXcodeBase(remoteOk, localOk);
        const std::string ladderProxy = fpgaLadderProxyFromEnv();
        const std::string helper = fpga ? ladderProxy : xcode;
        std::string ytId;
        const bool ytSrc = ytCastId(url, ytId);
        if (ytSrc) {
            std::string clockErr;
            misterplex::WallClockSource clockSrc = misterplex::WallClockSource::None;
            const auto clock = misterplex::waitForWallClockRescue(misterplex::kWallClockRescueWaitMs, &clockErr, &clockSrc);
            if (clock == misterplex::WallClockStatus::Unavailable) {
                // Not the shared 5-minute clock limiter: a refusal during gate 8
                // must still be visible after a rescue line.
                std::fprintf(stderr, "misterplexd: GCAST youtube refused: %s\n", clockErr.c_str());
                gcastArbiter.release(owner, tok);
                return false;
            }
            if (clock == misterplex::WallClockStatus::Stepped)
                misterplex::logClockSteppedOnce(misterplex::wallClockSourceName(clockSrc));
        }
        // AirPlay PCM, YouTube-on-OB, and standalone YouTube-on-FPGA. See planGcastPlay.
        GcastRouteInput route;
        route.url = url;
        route.fpga = fpga;
        route.ladder = ladderProxy;
        route.xcode = xcode;
        route.helper = helper;
        route.w = lw;
        route.h = lh;
        route.brKbps = br;
        route.startMs = startMs;
        route.directWanted = misterplex::envFlagWanted("GCAST_DIRECT");
        int pyMaj = 0, pyMin = 0;
        std::string ytdlpPref;
        if (const char* e = std::getenv("MPX_YTDLP_BIN"); e && *e)
            ytdlpPref = e;
        else
            ytdlpPref = loadConf(confPath, "YTDLP_BIN");
        const char* ytdlpPrefP = ytdlpPref.empty() ? nullptr : ytdlpPref.c_str();
        if (ytSrc) {
            yt_dlp::imagePython(pyMaj, pyMin);
            route.localYtdlp =
                yt_dlp::findBinary(pyMaj, pyMin, yt_dlp::candidatePaths(), ytdlpPrefP).kind !=
                yt_dlp::ReleaseKind::None;
            // B2: a running on-card helper must not take YouTube. Only a healthy
            // remote GCAST_XCODE does. Local yt-dlp stays the rc3 path.
            route.xcode = misterplex::gcast::youtubeXcodeBase(remoteOk, route.localYtdlp, xcode);
            if (route.xcode.empty())
                route.helper.clear();
            if (xcode.empty() && !route.localYtdlp)
                std::fprintf(stderr, "misterplexd: yt-dlp not executable (looked in %s)\n",
                             yt_dlp::searchedPaths().c_str());
        }
        CastPlan plan = planGcastPlay(route);
        yt_dlp::ResolveOptions yopt;
        if (ytSrc && plan.playable.compare(0, 9, "yt-local:") == 0) {
            const yt_dlp::FoundBinary bin = yt_dlp::findBinary(pyMaj, pyMin, yt_dlp::candidatePaths(), ytdlpPrefP);
            yopt.bin = bin.path;
            yopt.zipapp = bin.kind == yt_dlp::ReleaseKind::Zipapp;
            yopt.videoId = ytId;
            const yt_dlp::ResolveResult yr = yt_dlp::resolveVideo(yopt);
            std::fprintf(stderr, "misterplexd: yt-dlp resolve ok=%d peak_rss_kb=%u %s\n", yr.ok ? 1 : 0,
                         yr.peakRssKb, yr.ok ? yr.url.c_str() : yr.error.c_str());
            if (!yr.ok) {
                if (yr.extractionError)
                    yt_dlp::scheduleDailyUpdate(yt_dlp::configFromEnv());
                const auto fail = castGlassAction(GlassStep::ResolveFailed, player.playing(), !ytId.empty(),
                                                  owner, gcastArbiter.owner());
                if (fail.stopMedia) {
                    std::lock_guard<std::mutex> handoff(playHandoffMu);
                    if (gen == playGen.load())
                        misterplex::retirePlayback(player, pmsTimeline);
                }
                gcastArbiter.release(owner, tok);
                return false;
            }
            plan.playable = yr.audioUrl.empty() ? yr.url : ytAvPlayable(yr.url, yr.audioUrl);
        }
        if (plan.route == Route::Helper && fpga)
            plan.playable += "&fps=src";
        std::fprintf(stderr, "misterplexd: GCAST %s play gen=%llu backend=%s raster=%dx%d br=%d start=%lld route=%d (%s)\n",
                     ownerName(owner), static_cast<unsigned long long>(gen), fpga ? "fpga" : "owenb", lw, lh, br,
                     static_cast<long long>(startMs), static_cast<int>(plan.route), plan.why.c_str());
        if (plan.route == Route::Refuse) {
            gcastArbiter.release(owner, tok);
            return false;
        }
        // FIELD-1018 F4: FairPlay / Apple TV+ never plays; say so on screen now instead of a 30 s probe + open timeout.
        auto castRefuse = [&](const char* msg) {
            std::fprintf(stderr, "misterplexd: GCAST %s refused gen=%llu: %s\n", ownerName(owner),
                         static_cast<unsigned long long>(gen), msg);
            gcastArbiter.release(owner, tok);
            const std::string ready = misterplex::gcast_osd::gcastReadyLine(gcastApReady.load(), gcastYtUp.load());
            player.setIdleReadyLine(msg);
            std::thread([&player, ready, gen, &playGen] {
                std::this_thread::sleep_for(std::chrono::seconds(10));
                if (gen == playGen.load())
                    player.setIdleReadyLine(ready);
            }).detach();
            return false;
        };
        if (owner == Owner::AirPlay && castSourceVerdict(url, false, false) == CastSourceVerdict::Protected)
            return castRefuse(kProtectedMsg);
        ProbeInfo pi;
        // AirPlay probes the helper (remote GCAST_XCODE, else the on-card loopback).
        // YouTube probes only when it is actually using that helper. The local
        // yt-dlp path skips /probe so the on-card helper is not a second resolve.
        const std::string probeAt = ytSrc ? route.xcode : xcode;
        if (!probeAt.empty())
            pi = parseProbe(misterplex::plexHttpGet(probeAt + "/probe?src=" + encodeComponent(url), 90));
        if (owner == Owner::AirPlay && !xcode.empty() &&
            castSourceVerdict(url, true, pi.ok) == CastSourceVerdict::Unplayable)
            return castRefuse(kUnplayableMsg);
        const int64_t durMs = pi.durMs > 0 ? pi.durMs : durHintMs;
        std::fprintf(stderr, "misterplexd: GCAST probe ok=%d src=%dx%d vcodec=%s audio=%d dur=%lld\n", pi.ok ? 1 : 0,
                     pi.w, pi.h, pi.vcodec.c_str(), pi.audio ? 1 : 0, static_cast<long long>(durMs));
        std::unique_lock<std::mutex> handoff(playHandoffMu);
        if (gen != playGen.load()) {
            std::fprintf(stderr, "misterplexd: GCAST play superseded gen=%llu\n", static_cast<unsigned long long>(gen));
            return false;
        }
        misterplex::retirePlayback(player, pmsTimeline);
        FpgaWorkerHandoff fpgaWorkers(player);
        if (fpga)
            player.setDecodeSize(contentRes.width, contentRes.height);
        else
            player.setDecodeSize(bankRes.width, bankRes.height);
        player.setContentFpsRational(0, 0);
        // P4: now-playing names the owner ("YouTube: <title>"); the OSD shows it on pause/seek.
        player.setPlaybackTitle(misterplex::gcast_osd::gcastNowPlaying(ownerName(owner), !pi.title.empty() ? pi.title : title));
        player.setSourceTranscoded(plan.route == Route::Helper);
        const bool padded = plan.route == Route::Helper && !fpga;   // gcast-xcode letterboxes into lw x lh, SAR 1
        misterplex::SourceAspect aspect =
            padded ? misterplex::reduceSourceAspect(lw, lh)
                   : (pi.ok ? misterplex::reduceSourceAspect(pi.w, pi.h)
                            : misterplex::defaultSourceAspectForBank(bankRes.width, bankRes.height));
        if (!player.setSourceAspect(aspect)) {
            std::fprintf(stderr, "misterplexd: GCAST source aspect %u:%u not ACKed; retry bank default\n",
                         aspect.x, aspect.y);
            if (!player.setSourceAspect(misterplex::defaultSourceAspectForBank(player.decodeW(), player.decodeH()))) {
                gcastArbiter.release(owner, tok);
                return false;
            }
        }
        player.setSourceMediaSize(padded ? lw : pi.w, padded ? lh : pi.h);
        player.setSourceInterlaced(false);
        player.setSourceHasAudio(pi.ok ? pi.audio : true);
        activePlayGen.store(gen);
        misterplex::gate::GatePlayContext gateCtx;
        gateCtx.helperStream = plan.route == Route::Helper && fpga;
        gateCtx.helperConfigured = misterplex::ladderProxyConfigured();
        std::fprintf(stderr, "misterplexd: GCAST PLAY %s off=%lld dur=%lld\n",
                     misterplex::redactUrlTokens(plan.playable).c_str(), static_cast<long long>(startMs),
                     static_cast<long long>(durMs));
        if (owner == Owner::YouTube) {
            player.armCastOpenRetry(resumeMs);
            std::string yid = ytId;
            yt_dlp::ResolveOptions yAgain = yopt;
            player.armYt403Reresolve([yid, yAgain](std::string& playUrl, std::string& audio) mutable {
                yAgain.videoId = yid;
                const yt_dlp::ResolveResult again = yt_dlp::resolveVideo(yAgain);
                if (!again.ok)
                    return false;
                playUrl = again.url;
                audio = again.audioUrl;
                return true;
            });
        } else {
            player.armYt403Reresolve(nullptr);
        }
        if (!player.play(plan.playable, startMs, {}, durMs, gen, gateCtx)) {
            std::fprintf(stderr, "misterplexd: GCAST PLAY failed to start demux\n");
            gcastArbiter.release(owner, tok);
            return false;
        }
        r1PlayHold.started();
        fpgaWorkers.completePlayback();
        return true;
    };
    // A cast's own stop (sender /stop, local B/Esc while a cast owns the player).
    // Gate 2b: stopVideo arrives with an empty Lounge id after a failed resolve
    // already released the arbiter. Stop anyway when media is still active.
    // A different owner has since claimed the player; leave that play alone.
    auto castStop = [&](misterplex::gcast::Owner owner) {
        using misterplex::gcast::GlassStep;
        using misterplex::gcast::castGlassAction;
        const auto cur = gcastArbiter.owner();
        const bool playGenMatch = activePlayGen.load() == castGen.load();
        const auto act = castGlassAction(GlassStep::Stop, player.playing(), false, owner, cur, playGenMatch);
        if (!act.stopMedia)
            return;
        std::lock_guard<std::mutex> handoff(playHandoffMu);
        misterplex::retirePlayback(player, pmsTimeline);
        if (cur == owner)
            gcastArbiter.release(owner, castToken.load());
        std::fprintf(stderr, "misterplexd: GCAST %s stop\n", misterplex::gcast::ownerName(owner));
    };

    // Live content-res change (OSD O[5:4]): sync conf, retarget PMS weak ladder,
    // restart session at same playhead. Display (O[15:14]) writes video_mode in
    // applyOsd and only upserts DISPLAY_RES here — no DECODE / play restart.
    player.setLibraryFetch([&](const std::string& path) {
        std::string url = defaultPms;
        if (!path.empty() && path[0] == '/')
            url += path;
        else if (!path.empty())
            url += "/" + path;
        if (!confToken.empty())
            url += (url.find('?') == std::string::npos ? "?" : "&") +
                   std::string("X-Plex-Token=") + confToken;
        return misterplex::plexHttpGet(url);
    });
    player.setLibraryPlay([&](const std::string& rk) {
        misterplex::PlayRequest req;
        req.key = misterplex::librarylist::browseListPlayMediaKey(rk);
        req.ratingKey = misterplex::librarylist::browseListPlayRatingKey(rk);
        req.token = confToken;
        std::fprintf(stderr, "misterplexd: library overlay play ratingKey=%s\n", rk.c_str());
        {
            std::lock_guard<std::mutex> lock(sessionMu);
            lastPlay = req;
            if (!req.token.empty())
                lastToken = req.token;
        }
        comp.stagePlay(req);
        {
            std::lock_guard<std::mutex> handoff(playHandoffMu);
            player.r1BeginPlayRequest([&] { return ++playGen; });   // R1 C1: held from here
        }
        // doPlay -> stop() / FpgaWorkerHandoff joins OSD+input; never run on those threads.
        std::thread([&, req]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            doPlay(req);
        }).detach();
    });
    // MPX_BROWSE_V2 browse play (browse_controller.hpp): the request a phone cast makes. The
    // ticket's server (empty: the conf PMS and token) and its start: offsetPresent makes
    // doPlay skip the PMS viewOffset, because the RESUME / PLAY FROM START row chose already.
    // Remote rows resolve on their own server like a cast, so the ladder, the source rate
    // and the owner's timeline all come from doPlay unchanged. Never logs the token.
    if (browseV2Setting == misterplex::BrowseV2Setting::On) {
        std::lock_guard<std::mutex> lk(browsePlayMu);
        browsePlay = [&](const misterplex::BrowsePlayTicket& t) -> uint64_t {
            misterplex::PlayRequest req;
            req.ratingKey = t.ratingKey;
            req.key = t.key.empty() ? "/library/metadata/" + t.ratingKey : t.key;
            req.token = t.token.empty() ? confToken : t.token;
            req.address = t.address;
            req.port = t.port;
            req.protocol = t.protocol;
            req.serverMachineId = t.machineIdentifier;
            req.offsetMs = t.offsetMs;
            req.offsetPresent = t.offsetPresent;
            if (!t.ladderSrc.empty() || !t.ladderInput.empty()) {
                req.ladderSrc = t.ladderSrc;
                req.ladderInput = t.ladderInput;
                req.ladderSrcFor = req.key;
                req.ladderSrcOffsetMs = t.offsetPresent ? t.offsetMs : 0;
                req.ladderTicketUrl = t.ticketUrl;
            }
            {
                std::lock_guard<std::mutex> lock(sessionMu);
                lastPlay = req;
                if (!req.token.empty())
                    lastToken = req.token;
            }
            comp.stagePlay(req);
            uint64_t gen = 0;
            {
                std::lock_guard<std::mutex> handoff(playHandoffMu);
                gen = player.r1BeginPlayRequest([&] { return ++playGen; });   // R1 C1
            }
            req.dispatchGeneration = gen;
            std::fprintf(stderr,
                         "misterplexd: browse play gen=%llu ratingKey=%s source=%s start=%s "
                         "server=%s\n",
                         static_cast<unsigned long long>(gen), req.ratingKey.c_str(),
                         t.source.c_str(),
                         req.offsetPresent ? std::to_string(req.offsetMs).c_str() : "pms",
                         req.address.empty() ? "conf" : (req.address + ":" + req.port).c_str());
            std::thread([&, req]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                doPlay(req);
            }).detach();
            return gen;
        };
    }
    player.setOnDisplayResolutionChanged(
        [&](const misterplex::ContentResolution& dr) {
            if (!upsertConfKey(confPath, "DISPLAY_RES", dr.label))
                std::fprintf(stderr, "misterplexd: conf upsert DISPLAY_RES failed\n");
            else
                std::fprintf(stderr,
                             "misterplexd: DISPLAY_RES=%s saved (applies on reset; "
                             "raster unchanged until then)\n",
                             dr.label);
        });
    player.setOnContentResolutionChanged(
        [&](const misterplex::ContentResolution& cr, bool playingNow) {
            const auto displayRes = displayResolutionForNextPlay();
            std::fprintf(stderr,
                         "misterplexd: content_res OSD→%s (%dx%d) display=%s playing=%d — "
                         "PMS ladder + conf sync (PRESENT=fpga DDR path)\n",
                         cr.label, cr.width, cr.height, displayRes.label, playingNow ? 1 : 0);
            persistOsdResToConf(cr, displayRes);
            if (!playingNow)
                return;
            misterplex::PlayRequest cur;
            {
                std::lock_guard<std::mutex> lk(sessionMu);
                cur = lastPlay;
            }
            if (cur.key.empty() && cur.ratingKey.empty()) {
                std::fprintf(stderr,
                             "misterplexd: content_res change ignored — no bound session\n");
                return;
            }
            const int64_t pos = player.positionMs();
            cur.offsetMs = pos > 0 ? pos : cur.offsetMs;
            cur.offsetPresent = true;
            cur.dispatchGeneration = 0;
            std::fprintf(stderr,
                         "misterplexd: content_res live restart key=%s offset_ms=%lld → "
                         "content=%s display=%s\n",
                         cur.key.empty() ? cur.ratingKey.c_str() : cur.key.c_str(),
                         static_cast<long long>(cur.offsetMs), cr.label, displayRes.label);
            // Async so we never restart from inside the OSD poller thread.
            std::thread([&, cur]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                doPlay(cur);
            }).detach();
        });

    // Shared play-queue step: delta=+1 (auto-next / skipNext), delta=-1 (skipPrevious).
    // Returns true when a new title was started via doPlay.
    auto tryQueueStep = [&](int delta, const char* tag,
                            const misterplex::Companion::CurrentFn& current =
                                misterplex::Companion::CurrentFn{},
                            const misterplex::PlayRequest* accepted = nullptr,
                            bool naturalEof = false) -> bool {
        if (delta == 0)
            return false;
        if (current && !current())
            return false;
        const uint64_t generation = playGen.load();
        const uint64_t seekGeneration = seekGen.load();
        const uint64_t epoch = player.playbackEpoch();
        // autoNext conf gates natural-EOF advance only; explicit skipNext/Prev always try.
        misterplex::PlayRequest cur;
        std::string base, token;
        {
            std::lock_guard<std::mutex> lock(sessionMu);
            cur = lastPlay;
            base = lastBase;
            token = lastToken;
        }
        if (accepted) {
            cur = *accepted;
            base = misterplex::buildPlexBase(cur.protocol, cur.address, cur.port, base);
            if (!cur.token.empty())
                token = cur.token;
        }
        std::string qref = cur.containerKey;
        if (qref.empty() && !cur.playQueueId.empty())
            qref = "/playQueues/" + cur.playQueueId;
        if (qref.empty() || qref.find("/playQueues/") == std::string::npos) {
            std::fprintf(stderr, "misterplexd: %s skip — no playQueue bound\n", tag);
            return false;
        }
        auto q = misterplex::fetchPlayQueue(qref, base, token, cur.key, cur.playQueueItemId);
        if (!q.ok) {
            std::fprintf(stderr, "misterplexd: %s queue fetch failed: %s\n", tag, q.detail.c_str());
            return false;
        }
        if (naturalEof && !misterplex::naturalEofAdvanceAllowed(autoNext, q)) {
            std::fprintf(stderr, "misterplexd: %s off (AUTO_NEXT=0) and the ended item is not a pre-roll\n", tag);
            return false;
        }
        const auto destOpt = misterplex::queueStepDest(q.currentIndex, delta, q.items.size());
        if (!destOpt) {
            if (q.currentIndex < 0) {
                std::fprintf(stderr,
                             "misterplexd: %s refused: current item not in queue "
                             "(index=%d size=%zu delta=%d)\n",
                             tag, q.currentIndex, q.items.size(), delta);
            } else {
                std::fprintf(stderr,
                             "misterplexd: %s — end of queue (index=%d size=%zu delta=%d)\n",
                             tag, q.currentIndex, q.items.size(), delta);
            }
            return false;
        }
        const int dest = *destOpt;
        const auto& item = q.items[static_cast<size_t>(dest)];
        misterplex::PlayRequest n = misterplex::queueStepRequest(cur, q, item, token, generation);
        std::fprintf(stderr, "misterplexd: %s → %s title=%s pqItem=%s (from %s)\n", tag, n.key.c_str(),
                     item.title.c_str(), n.playQueueItemId.empty() ? "-" : n.playQueueItemId.c_str(),
                     q.detail.c_str());   // #25 b1: the pass that found the current row
        // Stage scrubber key before resolve so bindMedia key-match accepts this
        // item (and Web sees queue advance immediately).
        if (!comp.stagePlay(n, [&] {
                if ((current && !current()) || seekGen.load() != seekGeneration ||
                    player.playbackEpoch() != epoch)
                    return false;
                uint64_t expected = generation;
                // R1 C1: the step's generation and its PLXH en=1 hold in one step.
                return player.r1BeginPlayRequest([&]() -> uint64_t {
                           return playGen.compare_exchange_strong(expected, n.dispatchGeneration)
                                      ? n.dispatchGeneration
                                      : 0;
                       }) != 0;
            }))
            return false;
        {
            std::lock_guard<std::mutex> lock(sessionMu);
            if (playGen.load() != n.dispatchGeneration)
                return false;
            lastPlay = n;
            if (!token.empty())
                lastToken = token;
        }
        doPlay(n);
        return true;
    };

    // Next-episode stub: on natural EOF, if playQueue has a next item, play it.
    auto tryAutoNext = [&](const misterplex::Companion::CurrentFn& current) -> bool {
        // BUG2-PREROLL: with AUTO_NEXT=0 only a pre-roll (type="clip") still advances (checked after the fetch).
        return tryQueueStep(+1, autoNext ? "auto-next" : "auto-next (pre-roll)", current, nullptr, true);
    };

    // SPS-gate L3: the gate fallback hand-off (rows R2/R6, 0 AUs written). The player skipped this play's terminal
    // report; re-play the SAME request (the one doPlay committed for that generation) once, with forceLadder, at the
    // refused play's start. The next generation is CLAIMED in one step with staging the item, as tryQueueStep does
    // (review fix): a newer PLAY or a STOP that already moved playGen wins, and that request reports its own state.
    // Runs on its own thread because the player calls this from the worker that doPlay's retire joins.
    player.setGateFallbackHandler([&](const misterplex::MediaPlayer::GateFallbackRequest& fallback) {
        misterplex::PlayRequest n;
        uint64_t committedGen = 0;
        {
            std::lock_guard<std::mutex> lock(sessionMu);
            n = committedPlay;
            committedGen = committedPlayGen;
        }
        const bool loweredRetry = fallback.row == "R6p";
        if (committedGen != fallback.generation || n.key.empty() || n.fallbackUsed ||
            (loweredRetry && n.pmsLowered)) {
            std::fprintf(stderr, "misterplexd: GATE fallback dropped (%s) gen=%llu committed=%llu key=%s\n",
                         n.fallbackUsed || n.pmsLowered ? "already the item's fallback" : "not the committed play",
                         static_cast<unsigned long long>(fallback.generation),
                         static_cast<unsigned long long>(committedGen), n.key.empty() ? "-" : n.key.c_str());
            return;
        }
        n.offsetMs = fallback.offsetMs;
        n.offsetPresent = true;
        if (loweredRetry) {
            // R16: width gate on Utility 240p needs 320x240 PMS, not a lowered 624x480 universal.
            if (fallback.reason.find("pic_width_in_mbs") != std::string::npos &&
                misterplex::CoreGate::process().pairedRaster() == "240p" &&
                misterplex::ladderProxyConfigured()) {
                n.forceLadder = true;
                n.fallbackUsed = true;
            } else {
                n.pmsLowered = true;   // R6p: the same PMS universal request, lowered once; no ladder helper
            }
        } else {
            n.forceLadder = true;
            n.fallbackUsed = true;
        }
        n.gateDetail = fallback.reason;
        n.dispatchGeneration = fallback.generation + 1;
        std::fprintf(stderr, "misterplexd: GATE fallback %s → re-resolve key=%s off=%lld forceLadder=1 reason=%s\n",
                     fallback.row.c_str(), n.key.c_str(), static_cast<long long>(n.offsetMs),
                     fallback.reason.c_str());
        std::thread([&, n, refused = fallback.generation] {
            uint64_t expected = refused;
            if (!comp.stagePlay(n, [&] {
                    return player.r1BeginPlayRequest([&]() -> uint64_t {
                               return playGen.compare_exchange_strong(expected, n.dispatchGeneration)
                                          ? n.dispatchGeneration
                                          : 0;
                           }) != 0;
                })) {
                std::fprintf(stderr, "misterplexd: GATE fallback dropped (superseded at claim) gen=%llu now=%llu "
                                     "key=%s\n",
                             static_cast<unsigned long long>(refused),
                             static_cast<unsigned long long>(playGen.load()), n.key.c_str());
                return;
            }
            {
                std::lock_guard<std::mutex> lock(sessionMu);
                if (playGen.load() != n.dispatchGeneration)
                    return;
                lastPlay = n;
            }
            doPlay(n);
        }).detach();
    });

    player.setObEntropyHook([&](bool cavlc, unsigned kbps, unsigned loadPct) {   // BUG1: per-PMS memory
        std::lock_guard<std::mutex> lk(obTraitsMu);
        if (obTraitsKey.empty())
            return;
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        if (obTraits.record(obTraitsKey, cavlc, static_cast<int>(kbps), static_cast<int>(loadPct), now)) {
            const bool ok = obTraits.save(obTraitsPath, now);
            std::fprintf(stderr, "misterplexd: OB PMS memory: %s transcoder %s%s%s\n", obTraitsKey.c_str(),
                         cavlc ? "CAVLC " : "CABAC (entry cleared)",
                         cavlc ? (std::to_string(kbps) + " kbps load " + std::to_string(loadPct) + "%").c_str() : "",
                         ok ? "" : " (save failed)");
        }
    });
    player.setProgress([&](const std::string& st, int64_t t, int64_t d) {
        const uint64_t generation = activePlayGen.load();
        if (generation != playGen.load())
            return;
        if (gcastOn && generation == castGen.load() && gcastArbiter.dispatchProgress(st, t, d))
            return;   // GCAST: a cast owns this play; never Plex timeline / companion state
        if (st == "ended") {
            pmsTimeline.endSession(t, d);
            const uint64_t epoch = player.playbackEpoch();
            const uint64_t seekGeneration = seekGen.load();
            const auto current = [&, generation, epoch, seekGeneration] {
                return playGen.load() == generation &&
                       player.playbackEpoch() == epoch && seekGen.load() == seekGeneration;
            };
            misterplex::handleNaturalEof(comp, autoNextInFlight, t, d, current,
                [&, current] { return tryAutoNext(current); },
                [](auto finish) { std::thread(std::move(finish)).detach(); });
            return;
        }
        if (st == "stopped") {
            pmsTimeline.endSession(t, d);
            if (player.fpgaH264Backend()) {
                comp.closedStop(t, d, true);
                return;
            }
        } else
            pmsTimeline.reportState(st, t, d);
        comp.setState(st, t, d);
    });

    // playMedia HTTP thread: bump playGen immediately so in-flight doPlay aborts
    // before the new onPlay_ thread even schedules (cast A→B race).
    // Must not take playHandoffMu here — that mutex is held across player.stop().
    // A 720p remux EOF used to leave audioPump in pipe_read; stop() never
    // returned; the next Play blocked this lambda and Plex Web spun forever.
    comp.setPlayQueued([&]() {
        if (gcastOn)
            gcastArbiter.claim(misterplex::gcast::Owner::Plex);   // last caster wins (Plex preempts a cast)
        // R1 C1 (review 07:30): the new generation and its PLXH en=1 hold in one step, before
        // the resolve; a release for an older generation can no longer land between them.
        return player.r1BeginPlayRequest([&] { return ++playGen; });
    });

    // Keep PMS /:/timeline auth in lock-step with cast-supplied tokens.
    comp.setTokenUpdate([&](const std::string& tok) {
        if (tok.empty())
            return;
        {
            std::lock_guard<std::mutex> lock(sessionMu);
            lastToken = tok;
        }
        pmsTimeline.updateToken(tok);
    });

    // Plant lastPlay immediately so skipNext/skipPrevious during async resolve use the
    // new cast's queue bind — not the previous title's lastPlay (P4-SCRUB race).
    if (plexUiOn) {
        comp.setPlexUiNavEnabled(true);
        comp.setPlexUiNav([&](const std::string& op) { player.plexUiCompanionNav(op); });
    }

    comp.setPlay([&](const misterplex::PlayRequest& req) {
        {
            std::lock_guard<std::mutex> lock(sessionMu);
            lastPlay = req;
            if (!req.token.empty())
                lastToken = req.token;
            // Prefer cast address as provisional base when present.
            // address=host:port plus port= must not become host:port:port.
            if (!req.address.empty())
                lastBase = misterplex::joinPlexHostPort(req.protocol, req.address, req.port);
        }
        doPlay(req);
    });

    std::mutex seekMu;
    // SPS-gate L3 (review fix): a seek re-plays the SAME item from the companion's transport request, which carries
    // no gate fields. Keep the item's fallback (forceLadder, fallbackUsed, gateDetail) from lastPlay when the key
    // matches, so a seek on a fallback play stays on the ladder helper instead of re-trying the refused stream.
    auto seekStart = [&](const misterplex::PlayRequest& media) {
        misterplex::PlayRequest m = media;
        {
            std::lock_guard<std::mutex> lock(sessionMu);
            if (!m.key.empty() && lastPlay.key == m.key && lastPlay.pmsLowered)
                m.pmsLowered = true;   // a seek on the lowered retry stays lowered
            if (!m.key.empty() && lastPlay.key == m.key && lastPlay.fallbackUsed) {
                m.forceLadder = lastPlay.forceLadder;
                m.fallbackUsed = lastPlay.fallbackUsed;
                m.gateDetail = lastPlay.gateDetail;
            }
            // a lab MiSTer scrub black screen: a seek replays the SAME item on the SAME server, so it
            // resolves with the token that started it (playMedia token=), never a controller
            // token picked up from the seek request (see castControlRefreshToken).
            if (!m.key.empty() && lastPlay.key == m.key && !lastPlay.token.empty() &&
                m.token != lastPlay.token) {
                std::fprintf(stderr, "misterplexd: seek keeps the playMedia server token\n");
                m.token = lastPlay.token;
            }
        }
        m.seekReplay = true;
        doPlay(m);
    };
    // OWENB-P1B T5: the cast volume (setParameters) -> the player (OB: the core's attenuation).
    comp.setVolume([&player](int volume) { player.setVolumePercent(volume); });
    misterplex::wirePlaybackControls(comp, player, pmsTimeline, transport, activePlayGen,
        playHandoffMu, seekMu, seekStart, [&](uint64_t stopGen) {
            // R1 C1: the STOP ends every PLAY request before its own generation; a PLAY (and a seek)
            // after the STOP keeps its hold (review 08:17 (1)).
            player.r1PlayRequestsEndedBefore(stopGen);
            // SPS-gate L3 b4: a USER stop clears the on-screen gate error (the player's own teardown never does).
            player.clearGateError("user stop", true);
            std::lock_guard<std::mutex> lock(sessionMu);
            lastPlay = misterplex::PlayRequest{};
            lastBase.clear();
            seekReuse = misterplex::SeekReuseEntry{};
        }, [](auto command) { std::thread(std::move(command)).detach(); });
    comp.setDeferStop([&] {
        return misterplex::stopShouldWaitForResolve(
            g_doPlayDepth.load(std::memory_order_acquire) > 0, player.playing());
    });
    // skipNext → play-queue advance (always tries; independent of AUTO_NEXT conf).
    // Empty / unbound queue = no-op log.
    comp.setSkipNext([&]() {
        if (autoNextInFlight.exchange(true))
            return;
        std::thread([&]() {
            try {
                if (!tryQueueStep(+1, "skipNext"))
                    std::fprintf(stderr, "misterplexd: skipNext — no next item\n");
            } catch (...) {
                std::fprintf(stderr, "misterplexd: skipNext exception\n");
            }
            autoNextInFlight.store(false);
        }).detach();
    });
    // skipPrevious — Plex-style:
    //   t > 3s  → restart current title @ 0
    //   t ≤ 3s  → previous playQueue item when bound; else restart @ 0 (if t>0) or no-op
    // Companion supplies the accepted pre-restart position and command identity.
    comp.setSkipPrevious([&](const misterplex::TransportRequest& request) {
        const int64_t t = request.positionMs;
        const auto current = [&, request] { return transport.current(request); };
        constexpr int64_t kRestartThresholdMs = 3000;
        if (t > kRestartThresholdMs) {
            std::fprintf(stderr, "misterplexd: skipPrevious restart@0 (t=%lld)\n",
                         static_cast<long long>(t));
            comp.seekTo(0, current);
            return;
        }
        // Near start: try queue previous (network). Guard concurrent skip/auto-next.
        if (autoNextInFlight.exchange(true))
            return;
        std::thread([&, t, request, current]() {
            try {
                if (!tryQueueStep(-1, "skipPrevious", current, &request.media)) {
                    if (t > 0) {
                        // Queue lookup is network-bound. If the user scrubbed away
                        // while it was in flight, do not clobber the new plant with
                        // a stale restart@0 (unit: plant 40s after skipPrev@1.5s).
                        const int64_t nowT = comp.timelineTimeMs();
                        const int64_t drift = nowT > t ? nowT - t : t - nowT;
                        if (drift > kRestartThresholdMs && nowT > kRestartThresholdMs) {
                            std::fprintf(stderr,
                                         "misterplexd: skipPrevious no prev — drop stale "
                                         "restart@0 (was t=%lld now=%lld)\n",
                                         static_cast<long long>(t),
                                         static_cast<long long>(nowT));
                        } else {
                            std::fprintf(stderr,
                                         "misterplexd: skipPrevious no prev — restart@0 (t=%lld)\n",
                                         static_cast<long long>(t));
                            comp.seekTo(0, current);
                        }
                    } else {
                        std::fprintf(stderr,
                                     "misterplexd: skipPrevious — no previous item (at 0)\n");
                    }
                }
            } catch (...) {
                std::fprintf(stderr, "misterplexd: skipPrevious exception\n");
            }
            autoNextInFlight.store(false);
        }).detach();
    });

    if (!comp.start()) {
        std::fprintf(stderr, "misterplexd: companion start failed\n");
        return 1;
    }
    // 178 L2: the play-only hold dropped when L1 went idle, and the next TERM
    // took :3005 down before the following leg. Stay up for the life of the listen.
    g_signalHold.store(true, std::memory_order_release);

    if (defaultPms.empty()) {
        std::fprintf(stderr,
                     "misterplexd: no default Plex server configured; set PLEX_BASE in %s "
                     "or pass --pms URL. Cast clients that include a server address can still "
                     "select a server per play.\n",
                     confPath.c_str());
    }
    std::fprintf(stderr,
                 "misterplexd: running name=%s id=%s port=%d pms=%s servers=%zu decode=%dx%d "
                 "weak=%s/%s@%dk h264=%s@L%d present=%s auto_next=%d subs=%s\n",
                 name.c_str(), machineId.c_str(), port,
                 defaultPms.empty() ? "(unset)" : defaultPms.c_str(), servers.size(),
                 decodeW, decodeH, weak.profileName.c_str(), weak.videoResolution.c_str(),
                 weak.maxVideoBitrateKbps, weak.h264Profile.c_str(), weak.h264Level,
                 presentMode.c_str(), autoNext ? 1 : 0, subtitleMode.c_str());
    for (size_t i = 0; i < servers.size(); ++i)
        std::fprintf(stderr, "misterplexd:   server[%zu]=%s%s\n", i, servers[i].c_str(),
                     i == 0 ? " (default)" : "");

    // Watchdog: an SPI critical section SIGSTOPs Main only long enough to prove
    // it is parked between its own transactions, but if that window is ever
    // leaked — a hang inside the section, a thread killed mid-flight — Main
    // stays stopped and the user loses F12 with no way back. Sweeping /proc
    // twice a second costs nothing and touches no SPI, so it can never make
    // things worse. This only ever sends SIGCONT: misterplexd does not start,
    // stop, or reload Main.
    // The same 200 ms tick re-reads the loaded core for the core gate.
    //
    // Status block heartbeat (OSD-MENU-SPEC-1002 section 9; status_heartbeat.hpp, ported from
    // 583885f5/1cb004e5 without their idle trim and watcher). Every 2 s, H (status byte 6) carries
    // the heartbeat toggle, the Plex server, the player and the source fps. It writes only while the
    // core gate is open on a statusBlock build row. The PMS probe thread (one TCP connect per 10 s)
    // starts the first time that happens and runs until exit; before that there is no probe.
    // MPX_STATUS_BLOCK_FORCE=1 (lab only) acts as if every recognised row had statusBlock.
    namespace hb = misterplex::status_heartbeat;
    using Clock = std::chrono::steady_clock;
    if (hb::statusBlockForceEnv(std::getenv("MPX_STATUS_BLOCK_FORCE"))) {
        player.setStatusBlockForce(true);
        std::fprintf(stderr, "misterplexd: LAB MPX_STATUS_BLOCK_FORCE=1: status block on EVERY "
                             "recognised build row (UIO_SET_STATUS2 byte H every 2 s); never in prod\n");
    }
    const misterplex::PmsEndpoint pmsEp = misterplex::parsePmsBase(defaultPms);
    std::atomic<int> pmsServer{static_cast<int>(hb::Server::Unknown)};
    std::atomic<bool> probeRun{true};
    std::thread probeThr;
    // The PMS reachability probe (3 s connect, every 10 s): the status block's heartbeat, and (OWENB-P1B T7)
    // the OB idle card's "Plex server unreachable" screen. Started once, by whichever needs it first.
    auto startPmsProbe = [&] {
        if (!pmsEp.family || probeThr.joinable())
            return;
        probeThr = std::thread([&] {
            while (probeRun.load()) {
                pmsServer.store(static_cast<int>(misterplex::tcpReachable(pmsEp, 3000) ? hb::Server::Connected
                                                                                       : hb::Server::Unreachable));
                for (int i = 0; i < 50 && probeRun.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });
    };
    bool hbOn = false, hbToggle = false, hbWritten = false, hbFailLogged = false;
    int hbRetries = 0;
    uint8_t hbLast = 0;
    auto nextBeat = Clock::now();
    unsigned tick = 0;
    // GCAST: arbiter hooks + AirPlay receiver (GAIRPLAY=1). Started before the main loop.
    misterplex::gcast::AirplayFrontend airplayFe;
    misterplex::gcast::YtFrontend ytFe;
    if (gcastOn) {
        using misterplex::gcast::Owner;
        gcastArbiter.setRetire(Owner::Plex, [&](Owner by) {
            if (!comp.wantPlay())
                return;
            std::fprintf(stderr, "misterplexd: GCAST %s preempts Plex\n", misterplex::gcast::ownerName(by));
            comp.closedStop(comp.timelineTimeMs(), player.durationMs(), true);
        });
        gcastArbiter.setRetire(Owner::AirPlay, [&](Owner by) {
            std::fprintf(stderr, "misterplexd: GCAST %s preempts AirPlay\n", misterplex::gcast::ownerName(by));
            airplayFe.snapshot().ended();
        });
        gcastArbiter.setProgress(Owner::AirPlay, [&](const std::string& st, int64_t t, int64_t d) {
            if (st == "stopped" && player.reopenPending())   // bounded reopen restarts the same cast
                return;
            airplayFe.snapshot().progress(st, t, d);
            if (st == "ended" || st == "stopped")
                gcastArbiter.release(Owner::AirPlay, castToken.load());
        });
        gcastArbiter.setRetire(Owner::YouTube, [&](Owner by) {
            std::fprintf(stderr, "misterplexd: GCAST %s preempts YouTube\n", misterplex::gcast::ownerName(by));
            ytFe.ended(false);
        });
        gcastArbiter.setProgress(Owner::YouTube, [&](const std::string& st, int64_t t, int64_t d) {
            if (st == "stopped" && player.reopenPending()) {   // bounded reopen restarts the same cast
                ytFe.report("buffering", t, d);
                return;
            }
            if (st == "ended" || st == "stopped") {
                gcastArbiter.release(Owner::YouTube, castToken.load());
                ytFe.ended(st == "ended");   // natural end may auto-advance the Lounge playlist
                return;
            }
            ytFe.report(st, t, d);
        });
    }
    if (gcastAirplay) {
        using misterplex::gcast::ApEventType;
        using misterplex::gcast::Owner;
        misterplex::gcast::AirplayConfig apCfg;
        apCfg.name = name;
        apCfg.hwSeed = machineId;
        apCfg.keyfile = "/media/fat/misterplex/airplay_key.pem";
        if (const char* p = std::getenv("GAIRPLAY_PORT"))
            apCfg.port = static_cast<uint16_t>(std::atoi(p));
        // RAOP audio: decoded PCM served on GAIRPLAY_AUDIO_PORT (default 7002, 0 = off) as /raop.wav
        apCfg.audioPeerUrl = misterplex::gcast::helperBaseFromEnv();
        if (const char* p = std::getenv("GAIRPLAY_AUDIO_PORT"))
            apCfg.audioPort = static_cast<uint16_t>(std::atoi(p));
        if (const char* p = std::getenv("GAIRPLAY_AUDIO_HOST"))
            apCfg.audioHost = p;
        gcastApUp = airplayFe.start(apCfg, [&](const misterplex::gcast::ApEvent& e) {
            switch (e.type) {
            case ApEventType::Play:
                if (!castPlay(Owner::AirPlay, e.url, e.value, misterplex::gcast::titleFromUrl(e.url, "AirPlay"), 0, -1))
                    airplayFe.snapshot().ended();
                break;
            case ApEventType::Scrub:
                if (gcastArbiter.owner() == Owner::AirPlay && e.value >= 0)
                    player.seekMs(static_cast<int64_t>(e.value * 1000.0), {}, castGen.load());
                break;
            case ApEventType::Rate:
                if (gcastArbiter.owner() == Owner::AirPlay) {
                    std::lock_guard<std::mutex> handoff(playHandoffMu);
                    if (e.value == 0.0)
                        player.pause();
                    else
                        player.resume();
                }
                break;
            case ApEventType::Stop:
                castStop(Owner::AirPlay);
                airplayFe.snapshot().ended();
                break;
            }
        });
    }
    if (gcastYt) {
        using misterplex::gcast::Owner;
        using misterplex::gcast::YtCmd;
        misterplex::gcast::YtConfig ytCfg;
        ytCfg.name = name;
        ytCfg.hwSeed = machineId;
        ytCfg.screenIdFile = "/media/fat/misterplex/yt_screen_id";
        if (const char* p = std::getenv("GYT_SCREEN_ID_FILE"))
            ytCfg.screenIdFile = p;
        if (const char* p = std::getenv("GYT_DIAL_PORT"))
            ytCfg.dialPort = static_cast<uint16_t>(std::atoi(p));
        ytFe.start(ytCfg, [&](const misterplex::gcast::YtCommand& c) {
            switch (c.kind) {
            case YtCmd::Play:
            case YtCmd::Seek:   // a re-play at the new start (yt.ts / ladder are not demux-seekable)
                // Gate 8: DNS may not be up at boot. Do not start a session until
                // Lounge has connected; the connect loop keeps retrying.
                if (!ytFe.loungeConnected()) {
                    std::fprintf(stderr, "misterplexd: GYT play dropped; Lounge not connected\n");
                    ytFe.ended(false);
                    break;
                }
                if (!castPlay(Owner::YouTube, "yt:" + c.videoId, static_cast<double>(c.ms) / 1000.0,
                              "YouTube", c.kind == YtCmd::Seek ? player.durationMs() : 0,
                              c.kind == YtCmd::Seek ? c.fromMs : -1, c.kind == YtCmd::Seek))
                    ytFe.ended(false);
                break;
            case YtCmd::Pause:
            case YtCmd::Resume:
                if (gcastArbiter.owner() == Owner::YouTube) {
                    std::lock_guard<std::mutex> handoff(playHandoffMu);
                    if (c.kind == YtCmd::Pause)
                        player.pause();
                    else
                        player.resume();
                }
                break;
            case YtCmd::Stop:
                castStop(Owner::YouTube);
                break;
            }
        });
    }

    // AirPlay is on the ready card only when the helper is up and its /probe
    // self-test passes. GCAST_XCODE overrides the on-card loopback helper.
    // With neither, the card stays "Ready: Plex | YouTube".
    auto airplayReady = [&]() {
        if (!gcastApUp)
            return false;
        const std::string remote = misterplex::gcast::helperBaseFromEnv();
        if (!remote.empty())
            return misterplex::gcast::cachedHelperProbeSelfTest(remote);
        if (!misterplex::gcast::cachedLoopbackHelperHealth())
            return false;
        return misterplex::gcast::cachedHelperProbeSelfTest(misterplex::gcast::kLoopbackHelperBase);
    };
    bool gcastApShown = false;
    if (gcastOn) {   // P4: "Ready: Plex | AirPlay | YouTube" on the idle screens (flags off: nothing set)
        // YouTube stays off this line until Lounge connects (gcastYtUp updated below).
        gcastApShown = airplayReady();
        gcastApReady.store(gcastApShown);
        const std::string ready = misterplex::gcast_osd::gcastReadyLine(gcastApShown, gcastYtUp.load());
        player.setIdleReadyLine(ready);
        std::fprintf(stderr, "misterplexd: GCAST idle ready line \"%s\"\n", ready.c_str());
    }
    while (!g_stop.load()) {
        const bool ytNow = gcastYt && ytFe.loungeConnected();
        const bool apNow = airplayReady();
        if (ytNow != gcastYtUp.load() || apNow != gcastApShown) {
            gcastYtUp.store(ytNow);
            gcastApShown = apNow;
            gcastApReady.store(gcastApShown);
            const std::string ready = misterplex::gcast_osd::gcastReadyLine(gcastApShown, gcastYtUp.load());
            player.setIdleReadyLine(ready);
            std::fprintf(stderr, "misterplexd: GCAST idle ready line \"%s\"\n", ready.c_str());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (g_termHeld.load(std::memory_order_acquire) &&
            !g_termHoldLogged.exchange(true, std::memory_order_acq_rel))
            std::fprintf(stderr,
                         "misterplexd: SIGTERM ignored; companion listen still owns :3005\n");
        player.coreGateTick();
        if (player.owenbIdleWanted()) {   // OWENB-P1B T7: the PMS screen on the OB idle card
            startPmsProbe();
            const int st = pmsServer.load();
            player.setObPmsState(st == static_cast<int>(hb::Server::Unreachable) ? 0
                                 : st == static_cast<int>(hb::Server::Connected) ? 1
                                                                                 : -1,
                                 pmsEp.label);
        }
        const auto now = Clock::now();
        if (now >= nextBeat) {
            nextBeat = now + std::chrono::milliseconds(hb::kTickMs);
            if (player.statusBlockActive()) {
                if (!hbOn) {
                    hbOn = true;
                    std::fprintf(stderr,
                                 "misterplexd: status block on: heartbeat H = status byte 6 every "
                                 "%d ms (PMS probe %s)\n",
                                 hb::kTickMs, pmsEp.family ? pmsEp.label.c_str() : "off: no numeric PLEX_BASE");
                    startPmsProbe();
                }
                const bool pairing = misterplex::CoreGate::process().idleErrorPaintAllowed();
                const bool playing = player.playing();
                const uint8_t h = hb::compose(
                    !hbToggle,
                    pairing ? hb::Server::Pairing : static_cast<hb::Server>(pmsServer.load()),
                    pairing ? false : playing,
                    pairing ? 0
                            : (playing ? hb::fpsClass(player.contentFpsNum(), player.contentFpsDen())
                                       : 0));
                if (player.writeStatusHeartbeat(h)) {
                    hbToggle = !hbToggle;
                    hbLast = h;
                    hbWritten = true;
                    hbFailLogged = false;
                    hbRetries = 0;
                } else {
                    // Main busy or no live echo: two retries 200 ms apart (same toggle), then back
                    // to the 2 s cadence, so a stuck SPI never pauses Main more often than a beat.
                    if (++hbRetries <= 2)
                        nextBeat = now;
                    else
                        hbRetries = 0;
                    if (!hbFailLogged) {
                        hbFailLogged = true;
                        std::fprintf(stderr, "misterplexd: status block write skipped: %s\n",
                                     player.fpgaLastError().c_str());
                    }
                }
            }
        }
        if (++tick % 3 != 0)
            continue;
        misterplex::FpgaSpi::resumeStrandedMain();
    }
    player.recoverStrandedMenuCore(false);
    misterplex::stopWallClockRescue();
    probeRun.store(false);
    if (probeThr.joinable())
        probeThr.join();

    {
        std::lock_guard<std::mutex> handoff(playHandoffMu);
        misterplex::retirePlayback(player, pmsTimeline);
    }
    // Clean stop on a status block row: H[6:1] = 0 (server -, Idle, fps -), H[0] kept, so the core's
    // 360-field timeout shows Not running from the last beat.
    if (hbWritten && player.statusBlockActive())
        player.writeStatusHeartbeat(hb::stopValue(hbLast));
    airplayFe.stop();
    ytFe.stop();
    pmsTimeline.stopAndFlush();
    comp.stop();
    // Last chance on the way out: a window leaked during teardown would
    // otherwise outlive us.
    misterplex::FpgaSpi::resumeStrandedMain();
#ifdef MPX_HAVE_MAST_RELOAD
    if (g_mastReload.load()) {
        std::fprintf(stderr, "misterplexd: exiting %d (teardown %lld ms since the hand-off): the "
                             "MAST auto core reload launcher waits for this process to be gone, "
                             "reloads the core once (guarded), then restarts misterplexd\n",
                     misterplex::mast::kReloadExitCode, static_cast<long long>(mastTeardownMs()));
        return misterplex::mast::kReloadExitCode;
    }
#endif
    return 0;
}
