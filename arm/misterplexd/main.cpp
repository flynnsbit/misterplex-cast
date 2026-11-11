// misterplexd — ARM-side daemon for MiSTerPlex.
// Phase 2: GDM + companion + FFmpeg → /dev/fb0 (FPGA scanout via MiSTer_fb).
// Phase 4: multi-server conf, auto next-episode, optional subtitle burn-in.

#include "companion.hpp"
#include "natural_eof.hpp"
#include "playback_controls.hpp"
#include "gcast/airplay_fe.hpp"
#include "gcast/yt_fe.hpp"
#include "libmisterplex/delayed_task.hpp"
#include "libmisterplex/owenb_idle_card_ctl.hpp"
#include "libmisterplex/owenb_prefetch.hpp"
#include "gcast/cast_resolve.hpp"
#include "gcast/ready_wait.hpp"
#include "gcast/cast_glass.hpp"
#include "gcast/handoff_hold.hpp"
#include "gcast/yt_dlp.hpp"
#include "gcast/yt_live.hpp"
#include "gcast/yt_captions.hpp"
#include "gcast/dash_seek.hpp"
#include "gcast/protected_src.hpp"
#include "gcast/session_arbiter.hpp"
#include "wall_clock.hpp"
#include "libmisterplex/auto_picture_mode.hpp"
#include "libmisterplex/update_ed25519.hpp"
#include "libmisterplex/update_realfs.hpp"
#include "libmisterplex/update_wire.hpp"
#include "tls_http.hpp"
#include "spelunkr/aspect.hpp"
#include "spelunkr/archive_resolve.hpp"
#include "spelunkr/dead_run.hpp"
#include "spelunkr/fe.hpp"
#include "spelunkr/header_cache.hpp"
#include "spelunkr/prefetch_timeout.hpp"
#include "libmisterplex/tls_policy.hpp"
#include "present_retry.hpp"
#include "media_player.hpp"
#include "pms_timeline.hpp"
#include "plex_resolve.hpp"
#include "libmisterplex/mark_watched.hpp"
#include "libmisterplex/burn_subs_fix.hpp"
#include "libmisterplex/opensubs_fallback.hpp"
#include "libmisterplex/http_range_resume.hpp"
#include "libmisterplex/forced_subs.hpp"
#include "libmisterplex/remember_tracks.hpp"
#include "libmisterplex/remember_picture.hpp"
#include "libmisterplex/scrub_thumbs.hpp"
#include "libmisterplex/setstreams.hpp"
#include "libmisterplex/picture_14_9.hpp"
#include "libmisterplex/report_code.hpp"
#include "libmisterplex/ambient.hpp"
#include "libmisterplex/link_discover.hpp"
#include "libmisterplex/plex_web_client.hpp"
#include "libmisterplex/zaparoo.hpp"
#include "libmisterplex/plexui/filter_4_3.hpp"
#include "libmisterplex/in_play_options.hpp"
#include "libmisterplex/watched_badges.hpp"
#include "libmisterplex/crt_look.hpp"
#include "libmisterplex/idle_menu_key.hpp"
#include "libmisterplex/theme_music.hpp"
#include "libmisterplex/remote_h264_transcode.hpp"
#include "libmisterplex/lan_auth.hpp"
#include "libmisterplex/plexui/az_jump.hpp"
#include "libmisterplex/plexui/grid_page.hpp"
#include "libmisterplex/plexui/hide_empty_hubs.hpp"
#include "libmisterplex/plexui/plexui_home.hpp"
#include "libmisterplex/plexui/plexui_http.hpp"
#include "remote_start_probe.hpp"
#include "pms_reach.hpp"
#include "cast_token_flow.hpp"
#include "net_infra.hpp"
#include "seek_reuse.hpp"
#include "queue_step.hpp"
#include "next_item.hpp"
#include "libmisterplex/marker_flags.hpp"
#include "libmisterplex/plex_markers.hpp"
#include "transcode_offset.hpp"
#include "libmisterplex/gcast_osd.hpp"
#include "libmisterplex/display_raster.hpp"
#include "libmisterplex/pause_dim.hpp"
#include "libmisterplex/subtitle_position.hpp"
#include "libmisterplex/soft_subs.hpp"
#include "libmisterplex/av_clock.hpp"
#include "libmisterplex/core_gate.hpp"
#include "libmisterplex/owenb_pms_traits.hpp"
#include "libmisterplex/daemon_signal.hpp"
#include "libmisterplex/daemon_version.hpp"
#include "libmisterplex/d52_log.hpp"
#include "libmisterplex/h264_filter_setting.hpp"
#include "libmisterplex/legacy_core_prefix.hpp"
#include "libmisterplex/net_resume.hpp"
#include "libmisterplex/browse_v2.hpp"
#include "libmisterplex/pms_reach.hpp"
#include "libmisterplex/status_heartbeat.hpp"
#include "libmisterplex/status_facts.hpp"
#include "libmisterplex/phone_status_http.hpp"
#include "log_redact.hpp"
#include "libmisterplex/plexui/plexui_config.hpp"
#include "libmisterplex/plexui/plexui_link.hpp"
#include "queue_add_pms.hpp"
#include "libmisterplex/overscan_safe.hpp"
#include "libmisterplex/ui_safe_layout.hpp"
#include "lock_paths.hpp"
#include "mem_stat.hpp"
#include "lab_endpoints.hpp"
#include <functional>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <csignal>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fstream>
#include <ctime>
#include <mutex>
#include <cctype>
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
// lets the exit line report the teardown (FPGA Bug 21:52: it must end well inside the
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
    // One writer. DISPLAY_RES and the sign-in thread both call this.
    static std::mutex confWriteMu;
    static std::atomic<unsigned> confWriteSeq{0};
    std::lock_guard<std::mutex> lk(confWriteMu);
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
    const std::string tmp = path + ".tmp." + std::to_string(static_cast<long long>(::getpid())) + "." +
                            std::to_string(confWriteSeq.fetch_add(1));
    const int fd = ::open(tmp.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0)
        return false;
    if (::fchmod(fd, 0600) != 0) {
        ::close(fd);
        ::unlink(tmp.c_str());
        return false;
    }
    std::string payload;
    for (const auto& l : lines) {
        payload += l;
        payload += '\n';
    }
    size_t off = 0;
    while (off < payload.size()) {
        const ssize_t n = ::write(fd, payload.data() + off, payload.size() - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        off += static_cast<size_t>(n);
    }
    const int syncRc = ::fsync(fd);
    const int closeRc = ::close(fd);
    if (syncRc != 0 || closeRc != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    std::string dir = ".";
    const size_t slash = path.find_last_of('/');
    if (slash == 0)
        dir = "/";
    else if (slash != std::string::npos)
        dir = path.substr(0, slash);
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    return true;
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

namespace {

extern "C" {
unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* channels_in_file,
                                     int desired_channels);
void stbi_image_free(void* retval_from_stbi_load);
}

// Library art for the idle field. The token is on the GET only. Paths kept on
// the deck have no X-Plex-Token. Nothing here is uploaded.
std::shared_ptr<misterplex::AmbientDeck> fetchAmbientDeck(const std::string& base, const std::string& token) {
    auto deck = std::make_shared<misterplex::AmbientDeck>();
    if (base.empty() || token.empty() || g_stop.load())
        return deck;
    const std::string list = misterplex::ambientListPath();
    std::string url = base;
    url += list;
    const std::vector<std::pair<std::string, std::string>> auth = {{"X-Plex-Token", token}};   // #340: header
    const std::string xml = misterplex::plexHttpGet(url, 12, auth);
    if (g_stop.load())
        return deck;
    for (const misterplex::AmbientArt& meta : misterplex::parseAmbientCards(xml)) {
        if (g_stop.load() || static_cast<int>(deck->cards.size()) >= misterplex::kAmbientMaxCards)
            break;
        const std::string sized = misterplex::ambientSizedPath(meta.path);
        if (sized.empty())
            continue;
        std::string img = base;
        img += sized;
        const std::string body = misterplex::plexHttpGetBytes(img, 8, auth, misterplex::kAmbientByteCap);
        if (body.size() < 32)
            continue;
        int w = 0, h = 0, comp = 0;
        unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(body.data()),
                                                   static_cast<int>(body.size()), &w, &h, &comp, 4);
        if (!px)
            continue;
        misterplex::AmbientArt art;
        if (w > 0 && h > 0 && w <= 4096 && h <= 4096)
            art = misterplex::ambientPack(meta.ratingKey, meta.title, meta.path, px, w, h);
        stbi_image_free(px);
        if (art.visible())
            deck->cards.push_back(std::move(art));
    }
    return deck;
}

} // namespace

int main(int argc, char** argv) {
#ifdef MPX_SPELUNKR_BINARY
    if (::setenv("SPELUNKR", "1", 1) != 0) {
        std::fprintf(stderr, "misterplexd-spelunkr: cannot establish Spelunkr identity: %s\n", std::strerror(errno));
        return 1;
    }
#endif
    // GH#9: --version must not take the singleton or open FPGA (hung probe = color bars, no :3005).
    if (const int ver = misterplex::daemonVersionExit(argc, argv, stdout); ver >= 0)
        return ver;
    // idle-card:daemon-start -- NOT released here. tools/owenb_idle_card ("daemon not running") keeps the screen until
    // the daemon's ring opens (MediaPlayer::ensureOwenbPlayerLocked -> setBeforeRingOpen), so the core's colorbars
    // never show between daemon start and the "Ready to cast" card (glass ovn-2: ~11 s).
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
    std::mutex confTokenMu;
    auto readConfToken = [&]() -> std::string {
        std::lock_guard<std::mutex> lk(confTokenMu);
        return confToken;
    };
    auto writeConfToken = [&](std::string value) {
        std::lock_guard<std::mutex> lk(confTokenMu);
        confToken = std::move(value);
    };
    std::string plexTvToken;
    bool plexTvLinkEnabled = true;
    int decodeW = 320, decodeH = 240;
    bool cliDecodeExplicit = false;
    int cliDecodeW = 0, cliDecodeH = 0;
    std::string confDecodeValue;
    std::string presentMode = "fb0";
    std::string analogRasterConf; // ANALOG_RASTER=480i|240p|auto; unset/auto/invalid: no action
    misterplex::DdrFrameFormat ddrFrameFormat = misterplex::DdrFrameFormat::Yuv420p;
    bool ddrMemSync = true;
    bool ddrMemFlush = false;
    bool presentProfile = false;
    bool streamEnabled = false;
    std::string streamSkipRgb = "auto"; // auto | on | off — skip heavy RGB when PRESENT=fpga
    bool autoNext = true;
    misterplex::markers::Flags markerFlags;
    std::string subtitleMode = "off"; // off | burn | ffmpeg
    int subtitleStreamId = -1;
    // Phase 4 match-source-Hz: conf reserved for switchres; Content FPS hint is software-only.
    std::string matchSourceHz = "off";
    std::string sourceFpsConf = "auto";
    // AUTO_PICTURE_MODE=advise logs the source raster/cadence only. Default off.
    // The enabled mode never loads a core and never writes video_mode.
    std::string autoPictureConf = "off";
    std::string autoPictureMode = "off";  // resolved; non-empty env wins over conf
    misterplex::autopic::LastAdvice lastAutoPic;  // last decision, shown on the phone status page
    misterplex::WeakLadder weak;
    std::string cliTranscodeProfile;
    bool transcodeProfileExplicit = false;
    bool weakResExplicit = false;
    bool weakBitrateExplicit = false;
    bool weakQualityExplicit = false;
    bool weakH264ProfileExplicit = false;
    std::vector<std::string> servers;
    std::vector<misterplex::ConfServerRef> serverRefs;
    std::vector<misterplex::PlexBrowseTarget> browseTargets;
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
            std::printf("misterplexd [--version] [--name N] [--id ID] [--port N] [--ffmpeg PATH] [--pms URL] "
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

    // A linked box with no PLEX_BASE cannot browse. Discover the account's PMS
    // (owned first, local before remote) and remember the base URL. An
    // https://<ipv4> base is rewritten to that server's plex.direct name: the
    // raw IP is not on the certificate. The token is a header, and the log
    // line is the base with any query removed.
    auto fetchLinkedResources = [&](std::string& account) -> std::string {
        account = loadConf(confPath, "PLEXTV_TOKEN");
        auto typeForLog = [](std::string type, const std::string& secret) {
            const auto semi = type.find(';');
            if (semi != std::string::npos)
                type.resize(semi);
            while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back())))
                type.pop_back();
            if (type.empty() || (!secret.empty() && type.find(secret) != std::string::npos))
                return std::string("-");
            for (unsigned char c : type) {
                if (c <= 0x20 || c >= 0x7f || c == '%')
                    return std::string("-");
            }
            return type;
        };
        if (!misterplex::plexui::plexAccountTokenOk(account)) {
            account.clear();
            std::fprintf(stderr,
                         "misterplexd: linked resources http=0 bytes=0 type=- format=none servers=0\n");
            return {};
        }
        // This box's player id (the --id / machineId), not the shared product id.
        const misterplex::PlexHttpObserved got = misterplex::plexHttpGetObserved(
            "https://plex.tv/api/v2/resources?includeHttps=1&includeRelay=1", 8,
            {{"X-Plex-Token", account},
             {"Accept", "application/json"},
             {"X-Plex-Client-Identifier", misterplex::plexTvResourcesClientId(machineId)}});
        const std::string format = misterplex::resourcesBodyFormat(got.body);
        const std::size_t servers = misterplex::linkedServersFromResources(got.body).size();
        const std::string type = typeForLog(got.contentType, account);
        std::fprintf(stderr,
                     "misterplexd: linked resources http=%d bytes=%zu type=%s format=%s servers=%zu\n",
                     got.httpStatus, got.body.size(), type.c_str(), format.c_str(), servers);
        return got.body;
    };
    auto logLinkedBase = [&](const std::string& base, const std::string& account, const char* prefix) {
        const std::string shown = base.empty() ? std::string("(none)") : misterplex::linkBaseForLog(base);
        if (misterplex::plexui::logMentionsSecret(shown, account))
            std::fprintf(stderr, "misterplexd: %s (redacted)\n", prefix);
        else
            std::fprintf(stderr, "misterplexd: %s %s\n", prefix, shown.c_str());
    };
    std::vector<misterplex::LinkedServer> linkedRoster;
    auto rememberLinkedServers = [&](const std::string& body) {
        linkedRoster = misterplex::linkedServersFromResources(body);
        for (const misterplex::LinkedServer& s : linkedRoster) {
            const std::string line = misterplex::linkedServerLogLine(s);
            if ((!s.accessToken.empty() && line.find(s.accessToken) != std::string::npos) ||
                misterplex::plexui::logMentionsSecret(line, s.accessToken))
                std::fprintf(stderr, "misterplexd: linked server (redacted)\n");
            else
                std::fprintf(stderr, "misterplexd: linked server %s\n", line.c_str());
        }
        std::vector<std::string> ownedIds, ownedHosts;
        misterplex::ownedBurnIdsHosts(linkedRoster, ownedIds, ownedHosts);
        if (!ownedIds.empty() || !ownedHosts.empty()) {
            misterplex::burnPutOwners().set(ownedIds, ownedHosts);
            std::fprintf(stderr, "misterplexd: burn PUT owned servers=%zu (linked)\n", ownedIds.size());
        }
    };
    auto persistPlexBase = [&](const std::string& base, const char* why) -> bool {
        if (base.empty())
            return false;
        if (!upsertConfKey(confPath, "PLEX_BASE", base)) {
            std::fprintf(stderr, "misterplexd: failed to persist PLEX_BASE\n");
            return false;
        }
        defaultPms = base;
        servers.clear();
        servers.push_back(base);
        std::fprintf(stderr, "misterplexd: %s\n", why);
        return true;
    };
    auto applyDiscoveredBase = [&]() {
        const bool bareIp = misterplex::httpsBareIpv4(defaultPms);
        std::string account;
        const std::string body = fetchLinkedResources(account);
        if (account.empty())
            return;
        // Always keep the picker roster, including when PLEX_BASE is already set.
        rememberLinkedServers(body);
        if (defaultPms.empty()) {
            const std::string base = misterplex::chooseLinkedServerBase(body);
            logLinkedBase(base, account, "linked server");
            persistPlexBase(base, "PLEX_BASE set from the linked account");
            return;
        }
        if (!bareIp)
            return;
        const std::string repaired = misterplex::repairBareIpHttpsBase(defaultPms, body);
        if (repaired.empty())
            return;
        logLinkedBase(repaired, account, "https address rewritten to plex.direct");
        persistPlexBase(repaired, "PLEX_BASE https address rewritten to its plex.direct name");
    };
    {
        // Multi-server: PLEX_SERVERS=url1,url2 and/or repeated PLEX_BASE= lines.
        auto baseLines = loadConfAll(confPath, "PLEX_BASE");
        auto serversCsv = loadConf(confPath, "PLEX_SERVERS");
        serverRefs = misterplex::parsePlexServerConf(serversCsv);
        servers = misterplex::mergePlexServers(misterplex::plexServerConfUrls(serverRefs), baseLines);
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

        writeConfToken(loadConf(confPath, "PLEX_TOKEN"));
        plexTvToken = loadConf(confPath, "PLEXTV_TOKEN");
        if (const std::string tv = loadConf(confPath, "TLS_VERIFY"); !tv.empty())
            ::setenv("MPX_TLS_VERIFY", tv.c_str(), 0);
        if (const std::string trust = loadConf(confPath, "TLS_TRUST_FILE"); !trust.empty())
            ::setenv("MPX_TLS_TRUST_FILE", trust.c_str(), 0);
        misterplex::publishDefaultCaBundle();
        if (misterplex::tlsVerifyOff())
            misterplex::logTlsOptOutOnce();
        if (!misterplex::envFlagWanted("SPELUNKR") && !plexTvToken.empty())
            applyDiscoveredBase();
        // Clock rescue runs on its own thread so a down network at boot does not
        // consume the one shot. Order: rtc0, then SNTP, then HTTPS Date. While
        // the network is down the rescue polls at most once a second. The
        // 2/4/8/16/30 s backoff starts only after a network-ready SNTP and
        // HTTPS Date miss. YouTube and TLS wait on that thread.
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
        // ALLOW_480P60 default off. Env MPX_ALLOW_480P60 or ALLOW_480P60 wins.
        v = loadConf(confPath, "ALLOW_480P60");
        if (!v.empty() && !std::getenv("MPX_ALLOW_480P60") && !std::getenv("ALLOW_480P60"))
            setenv("MPX_ALLOW_480P60", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: ALLOW_480P60=%d\n",
                     misterplex::allow480p60Enabled() ? 1 : 0);
        // PAUSE_DIM default off. Env MPX_PAUSE_DIM or PAUSE_DIM wins.
        // Dims painted OSD/UI while paused, and a still PlexUI/library/menu
        // after PAUSE_DIM_IDLE_MIN (default 5). The FPGA picture is not scaled.
        v = loadConf(confPath, "PAUSE_DIM");
        if (!v.empty() && !std::getenv("MPX_PAUSE_DIM") && !std::getenv("PAUSE_DIM"))
            setenv("MPX_PAUSE_DIM", v.c_str(), 0);
        v = loadConf(confPath, "PAUSE_DIM_IDLE_MIN");
        if (!v.empty() && !std::getenv("MPX_PAUSE_DIM_IDLE_MIN") && !std::getenv("PAUSE_DIM_IDLE_MIN"))
            setenv("MPX_PAUSE_DIM_IDLE_MIN", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: PAUSE_DIM=%d\n",
                     misterplex::pauseDimEnabled() ? 1 : 0);
        // YT_NEXT_PREV default off. Env MPX_YT_NEXT_PREV or YT_NEXT_PREV wins.
        // On: lounge next/previous skips within the YouTube queue. Natural
        // end, play, pause, seek, and stop stay on their own path.
        v = loadConf(confPath, "YT_NEXT_PREV");
        if (!v.empty() && !std::getenv("MPX_YT_NEXT_PREV") && !std::getenv("YT_NEXT_PREV"))
            setenv("MPX_YT_NEXT_PREV", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: YT_NEXT_PREV=%d\n",
                     misterplex::gcast::lounge::ytNextPrevEnabled() ? 1 : 0);
        // YT_LIVE default off. Env MPX_YT_LIVE or YT_LIVE wins.
        // On: a YouTube livestream (or a premiere that has started) is
        // copy-remuxed from HLS to MPEG-TS. Off: the VOD selector and play
        // URL are unchanged. Next/prev, lounge queue, and Loading… are not.
        v = loadConf(confPath, "YT_LIVE");
        if (!v.empty() && !std::getenv("MPX_YT_LIVE") && !std::getenv("YT_LIVE"))
            setenv("MPX_YT_LIVE", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: YT_LIVE=%d\n",
                     misterplex::gcast::yt_live::ytLiveEnabled() ? 1 : 0);
        // YT_VOLUME default off. Env MPX_YT_VOLUME or YT_VOLUME wins.
        // On: lounge setVolume with an integer 0-100 calls player
        // setVolumePercent, and onVolumeChanged carries that level.
        // getVolume replies the last applied level (100 until a set).
        // Off: the reply stays 100 and the player volume path is unchanged.
        v = loadConf(confPath, "YT_VOLUME");
        if (!v.empty() && !std::getenv("MPX_YT_VOLUME") && !std::getenv("YT_VOLUME"))
            setenv("MPX_YT_VOLUME", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: YT_VOLUME=%d\n",
                     misterplex::gcast::lounge::ytVolumeEnabled() ? 1 : 0);
        // YT_CAST_TITLE default off. Env MPX_YT_CAST_TITLE or YT_CAST_TITLE wins.
        // On: the same local yt-dlp resolve prints the extract title and the OSD
        // / phone now-playing line uses it. Off: that argv is unchanged and the
        // title line stays on the tip path. No captions request.
        v = loadConf(confPath, "YT_CAST_TITLE");
        if (!v.empty() && !std::getenv("MPX_YT_CAST_TITLE") && !std::getenv("YT_CAST_TITLE"))
            setenv("MPX_YT_CAST_TITLE", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: YT_CAST_TITLE=%d\n",
                     misterplex::gcast::yt_dlp::ytCastTitleEnabled() ? 1 : 0);
        v = loadConf(confPath, "YT_CACHE_DIR");
        const std::string ytCache = misterplex::gcast::yt_dlp::ytCacheDir(v);
        setenv("MPX_YT_CACHE_DIR", ytCache.c_str(), 1);
        misterplex::gcast::yt_dlp::ensureYtCacheDir();
        for (const char* key : {"YT_WARM", "YT_WARM_SOCK", "YT_WARM_SCRIPT"}) {
            v = loadConf(confPath, key);
            const std::string env = std::string("MPX_") + key;
            if (!v.empty() && !std::getenv(env.c_str()) && !std::getenv(key))
                setenv(env.c_str(), v.c_str(), 0);
        }
        // YT_RESOLVE_TIMEOUT seconds (default 90, clamp 10-120). Env wins.
        v = loadConf(confPath, "YT_RESOLVE_TIMEOUT");
        if (!v.empty() && !std::getenv("MPX_YT_RESOLVE_TIMEOUT") && !std::getenv("YT_RESOLVE_TIMEOUT"))
            setenv("MPX_YT_RESOLVE_TIMEOUT", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: YT_RESOLVE_TIMEOUT=%dms\n",
                     misterplex::gcast::yt_dlp::resolveTimeoutMs());
        // YT_CAPTIONS default off. Env MPX_YT_CAPTIONS or YT_CAPTIONS wins.
        // On: lounge setSubtitlesTrack shows or hides that track on the HDMI
        // caption band. Off: the command stays unparsed, getSubtitlesTrack
        // still acks the video id only, and no caption text is painted.
        v = loadConf(confPath, "YT_CAPTIONS");
        if (!v.empty() && !std::getenv("MPX_YT_CAPTIONS") && !std::getenv("YT_CAPTIONS"))
            setenv("MPX_YT_CAPTIONS", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: YT_CAPTIONS=%d\n",
                     misterplex::gcast::lounge::ytCaptionsEnabled() ? 1 : 0);
        // MARK_WATCHED default off. Env MPX_MARK_WATCHED or MARK_WATCHED wins.
        // On: the program menu can mark watched, mark unwatched, or drop the
        // playing/focused item from Continue Watching. Those are separate PMS
        // requests. /:/timeline is unchanged. Off: the menu does not grow.
        v = loadConf(confPath, "MARK_WATCHED");
        if (!v.empty() && !std::getenv("MPX_MARK_WATCHED") && !std::getenv("MARK_WATCHED"))
            setenv("MPX_MARK_WATCHED", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: MARK_WATCHED=%d\n",
                     misterplex::markWatchedEnabled() ? 1 : 0);
        // GRID_PAGE default off. Env MPX_GRID_PAGE or GRID_PAGE wins.
        // On: the library grid requests the next 200 titles when you move past
        // the rows already loaded. Off: the first page is the whole grid.
        v = loadConf(confPath, "GRID_PAGE");
        if (!v.empty() && !std::getenv("MPX_GRID_PAGE") && !std::getenv("GRID_PAGE"))
            setenv("MPX_GRID_PAGE", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: GRID_PAGE=%d\n",
                     misterplex::gridPageEnabled() ? 1 : 0);
        // AZ_JUMP default off. Env MPX_AZ_JUMP or AZ_JUMP wins.
        // On: UP from a library's category chips opens an A–Z bar. Select loads
        // titles whose PMS firstCharacter is that letter. GRID_PAGE pages it.
        // Off: the library grid matches a build without this flag.
        v = loadConf(confPath, "AZ_JUMP");
        if (!v.empty() && !std::getenv("MPX_AZ_JUMP") && !std::getenv("AZ_JUMP"))
            setenv("MPX_AZ_JUMP", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: AZ_JUMP=%d\n",
                     misterplex::azJumpEnabled() ? 1 : 0);
        // SETSTREAMS default off. Env MPX_SETSTREAMS or SETSTREAMS wins.
        // On: the Program menu lists this play's subtitles and audio. A pick
        // re-resolves at the current offset (subtitle off, image/PGS burn,
        // audioStreamID) through the setStreams path already on this tip.
        // Off: the menu matches a build without this flag. Plex Web setStreams
        // is unchanged either way.
        v = loadConf(confPath, "SETSTREAMS");
        if (!v.empty() && !std::getenv("MPX_SETSTREAMS") && !std::getenv("SETSTREAMS"))
            setenv("MPX_SETSTREAMS", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: SETSTREAMS=%d\n",
                     misterplex::setStreamsEnabled() ? 1 : 0);
        // CHAPTERS default off. Env MPX_CHAPTERS or CHAPTERS wins.
        // On: the metadata GET adds includeChapters=1 and the Program menu lists
        // those PMS markers. Previous / Next / a row seek to startTimeOffset.
        // Off: that GET and the menu match a build without this flag.
        v = loadConf(confPath, "CHAPTERS");
        if (!v.empty() && !std::getenv("MPX_CHAPTERS") && !std::getenv("CHAPTERS"))
            setenv("MPX_CHAPTERS", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: CHAPTERS=%d\n",
                     misterplex::chaptersEnabled() ? 1 : 0);
        // FORCED_SUBS default off. Env MPX_FORCED_SUBS or FORCED_SUBS wins.
        // On: a play or re-resolve with no user subtitle pick selects a PMS
        // forced=1 stream (soft overlay, or the burn path already on this tip).
        // Off: that pick matches a build without this flag. SUBTITLE_FORCED is
        // unchanged.
        v = loadConf(confPath, "FORCED_SUBS");
        if (!v.empty() && !std::getenv("MPX_FORCED_SUBS") && !std::getenv("FORCED_SUBS"))
            setenv("MPX_FORCED_SUBS", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: FORCED_SUBS=%d\n",
                     misterplex::forcedSubsEnabled() ? 1 : 0);
        // REMEMBER_TRACKS default off. Env MPX_REMEMBER_TRACKS or REMEMBER_TRACKS wins.
        // On: a Program-menu or setStreams pick is stored in remember_tracks.txt and
        // applied on the next episode of that show. Off: the pick matches this tip.
        v = loadConf(confPath, "REMEMBER_TRACKS");
        if (!v.empty() && !std::getenv("MPX_REMEMBER_TRACKS") && !std::getenv("REMEMBER_TRACKS"))
            setenv("MPX_REMEMBER_TRACKS", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: REMEMBER_TRACKS=%d\n",
                     misterplex::rememberTracksEnabled() ? 1 : 0);
        // REMEMBER_PICTURE default off. Env MPX_REMEMBER_PICTURE or REMEMBER_PICTURE wins.
        // On: the Program menu picture row is stored in remember_picture.txt and
        // restored on the next episode of that show. Off: the menu and the fit match this tip.
        v = loadConf(confPath, "REMEMBER_PICTURE");
        if (!v.empty() && !std::getenv("MPX_REMEMBER_PICTURE") && !std::getenv("REMEMBER_PICTURE"))
            setenv("MPX_REMEMBER_PICTURE", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: REMEMBER_PICTURE=%d\n",
                     misterplex::rememberPictureEnabled() ? 1 : 0);
        // SCRUB_THUMBS default off. Env MPX_SCRUB_THUMBS or SCRUB_THUMBS wins.
        // On: a seek paints the nearest Plex BIF frame above the bar when the
        // part has an sd or hd index. Off: no fetch, and the seek bar matches
        // a build without this flag.
        v = loadConf(confPath, "SCRUB_THUMBS");
        if (!v.empty() && !std::getenv("MPX_SCRUB_THUMBS") && !std::getenv("SCRUB_THUMBS"))
            setenv("MPX_SCRUB_THUMBS", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: SCRUB_THUMBS=%d\n",
                     misterplex::scrubThumbsEnabled() ? 1 : 0);
        // PICTURE_14_9 default off. Env MPX_PICTURE_14_9 or PICTURE_14_9 wins.
        // On: a wide title's SPS display window is narrowed to 14:9 so the core's
        // existing Letterbox fit is the 14:9 compromise on a 4:3 tube. The Program
        // menu can put this play back to full. Off: Fit/Letterbox/Crop are unchanged.
        v = loadConf(confPath, "PICTURE_14_9");
        if (!v.empty() && !std::getenv("MPX_PICTURE_14_9") && !std::getenv("PICTURE_14_9"))
            setenv("MPX_PICTURE_14_9", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: PICTURE_14_9=%d\n",
                     misterplex::picture149Enabled() ? 1 : 0);
        // REPORT_CODE default off. Env MPX_REPORT_CODE or REPORT_CODE wins.
        // On: the Ready card and the phone status page show one short code,
        // and a redacted log pack is written on disk. Nothing is uploaded.
        // Off: the card, the page, and the disk match a build without this flag.
        v = loadConf(confPath, "REPORT_CODE");
        if (!v.empty() && !std::getenv("MPX_REPORT_CODE") && !std::getenv("REPORT_CODE"))
            setenv("MPX_REPORT_CODE", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: REPORT_CODE=%d\n",
                     misterplex::reportCodeEnabled() ? 1 : 0);
        // AMBIENT default off. Env MPX_AMBIENT or AMBIENT wins.
        // On: the OwenB idle field cycles library art behind the Ready card.
        // Off: that field stays black and there is no library GET.
        // IDLE_SCREEN is unchanged either way. Do not set this in STAGE conf.
        v = loadConf(confPath, "AMBIENT");
        if (!v.empty() && !std::getenv("MPX_AMBIENT") && !std::getenv("AMBIENT"))
            setenv("MPX_AMBIENT", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: AMBIENT=%d\n",
                     misterplex::ambientEnabled() ? 1 : 0);
        // ZAPAROO default off. Env MPX_ZAPAROO or ZAPAROO wins.
        // On: /misterplex/zaparoo?text=<card> starts that library item through
        // the cast play path. Off: the route refuses and does not play.
        // Do not set this in STAGE conf.
        v = loadConf(confPath, "ZAPAROO");
        if (!v.empty() && !std::getenv("MPX_ZAPAROO") && !std::getenv("ZAPAROO"))
            setenv("MPX_ZAPAROO", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: ZAPAROO=%d\n",
                     misterplex::zaparooEnabled() ? 1 : 0);
        // FILTER_4_3 default off. Env MPX_FILTER_4_3 or FILTER_4_3 wins.
        // On: the library chip row has a 4:3 toggle. Select hides titles
        // whose stored aspect is not about 4:3. Off: that chip is absent
        // and the grid is the fetched list. Do not set this in STAGE conf.
        v = loadConf(confPath, "FILTER_4_3");
        if (!v.empty() && !std::getenv("MPX_FILTER_4_3") && !std::getenv("FILTER_4_3"))
            setenv("MPX_FILTER_4_3", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: FILTER_4_3=%d\n",
                     misterplex::filter43Enabled() ? 1 : 0);
        // IN_PLAY_OPTIONS default off. Env MPX_IN_PLAY_OPTIONS or
        // IN_PLAY_OPTIONS wins. On: the Program menu gains Options during a
        // play (picture size, Play from start). Off: that row is absent.
        // Do not set this in STAGE conf.
        v = loadConf(confPath, "IN_PLAY_OPTIONS");
        if (!v.empty() && !std::getenv("MPX_IN_PLAY_OPTIONS") && !std::getenv("IN_PLAY_OPTIONS"))
            setenv("MPX_IN_PLAY_OPTIONS", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: IN_PLAY_OPTIONS=%d\n",
                     misterplex::inPlayOptionsEnabled() ? 1 : 0);
        // WATCHED_BADGES default off. Env MPX_WATCHED_BADGES or
        // WATCHED_BADGES wins. On: a finished title gets a check on its
        // poster. Off: posters are unchanged. Do not set this in STAGE conf.
        v = loadConf(confPath, "WATCHED_BADGES");
        if (!v.empty() && !std::getenv("MPX_WATCHED_BADGES") && !std::getenv("WATCHED_BADGES"))
            setenv("MPX_WATCHED_BADGES", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: WATCHED_BADGES=%d\n",
                     misterplex::watchedBadgesEnabled() ? 1 : 0);
        // HIDE_EMPTY_HUBS default on (#220). Env MPX_HIDE_EMPTY_HUBS or
        // HIDE_EMPTY_HUBS wins. On: Home hubs with no tiles are dropped.
        // Off (0): hubs are unchanged.
        v = loadConf(confPath, "HIDE_EMPTY_HUBS");
        if (!v.empty() && !std::getenv("MPX_HIDE_EMPTY_HUBS") && !std::getenv("HIDE_EMPTY_HUBS"))
            setenv("MPX_HIDE_EMPTY_HUBS", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: HIDE_EMPTY_HUBS=%d\n",
                     misterplex::plexui::hideEmptyHubsEnabled() ? 1 : 0);
        // CRT_LOOK default off. Env MPX_CRT_LOOK or CRT_LOOK wins.
        // On: the on-device UI canvas uses CRT_LOOK_PRESET (default
        // scanlines). Off: that canvas is unchanged. The picture plane
        // is not this flag. Do not set this in STAGE conf.
        v = loadConf(confPath, "CRT_LOOK");
        if (!v.empty() && !std::getenv("MPX_CRT_LOOK") && !std::getenv("CRT_LOOK"))
            setenv("MPX_CRT_LOOK", v.c_str(), 0);
        v = loadConf(confPath, "CRT_LOOK_PRESET");
        if (!v.empty() && !std::getenv("MPX_CRT_LOOK_PRESET") && !std::getenv("CRT_LOOK_PRESET"))
            setenv("MPX_CRT_LOOK_PRESET", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: CRT_LOOK=%d\n",
                     misterplex::crtLookEnabled() ? 1 : 0);
        if (misterplex::crtLookEnabled())
            std::fprintf(stderr, "misterplexd: CRT_LOOK preset=%s\n",
                         misterplex::crtLookPresetName(misterplex::crtLookSelected()));
        // IDLE_MENU_KEY default off. Env MPX_IDLE_MENU_KEY or IDLE_MENU_KEY wins.
        // On: USB keyboard P on the Ready idle screen opens the Program menu
        // (the same panel as pad Y). Off: P is unbound. M, Tab, and F12 are
        // unchanged. In a play and in PlexUI, P does nothing.
        // Do not set this in STAGE conf.
        v = loadConf(confPath, "IDLE_MENU_KEY");
        if (!v.empty() && !std::getenv("MPX_IDLE_MENU_KEY") && !std::getenv("IDLE_MENU_KEY"))
            setenv("MPX_IDLE_MENU_KEY", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: IDLE_MENU_KEY=%d\n",
                     misterplex::idleMenuKeyEnabled() ? 1 : 0);
        // THEME_MUSIC default off. Env MPX_THEME_MUSIC or THEME_MUSIC wins.
        // On: plexui Home plays a 0.5 s pad; Show Detail fetches theme bytes
        // with the token as a header (GH#235). Do not set this in STAGE conf.
        v = loadConf(confPath, "THEME_MUSIC");
        if (!v.empty() && !std::getenv("MPX_THEME_MUSIC") && !std::getenv("THEME_MUSIC"))
            setenv("MPX_THEME_MUSIC", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: THEME_MUSIC=%d\n",
                     misterplex::themeMusicEnabled() ? 1 : 0);
        // #207 REMOTE_H264_TRANSCODE default on: remote plays always PMS H.264 transcode.
        v = loadConf(confPath, "REMOTE_H264_TRANSCODE");
        if (!v.empty() && !std::getenv("MPX_REMOTE_H264_TRANSCODE") && !std::getenv("REMOTE_H264_TRANSCODE"))
            setenv("MPX_REMOTE_H264_TRANSCODE", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: REMOTE_H264_TRANSCODE=%d\n",
                     misterplex::remoteH264TranscodeEnabled() ? 1 : 0);
        // LAN_AUTH default off. Env MPX_LAN_AUTH or LAN_AUTH wins. On: /misterplex/*
        // control routes and the transcode helper need X-MPX-Auth (Plex companion
        // routes exempt). Secret: env MPX_LAN_AUTH_SECRET, else conf LAN_AUTH_SECRET,
        // else LAN_AUTH_SECRET_FILE, generated on the card when missing. Never logged.
        // Do not set this in STAGE conf.
        v = loadConf(confPath, "LAN_AUTH");
        if (!v.empty() && !std::getenv("MPX_LAN_AUTH") && !std::getenv("LAN_AUTH"))
            setenv("MPX_LAN_AUTH", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: LAN_AUTH=%d\n", misterplex::lan_auth::enabled() ? 1 : 0);
        if (misterplex::lan_auth::enabled()) {
            std::string file = loadConf(confPath, "LAN_AUTH_SECRET_FILE");
            if (const char* f = std::getenv("LAN_AUTH_SECRET_FILE"); f && f[0])
                file = f;
            if (file.empty())
                file = misterplex::lan_auth::kDefaultSecretFile;
            const bool ok = misterplex::lan_auth::adoptSecret(loadConf(confPath, "LAN_AUTH_SECRET"), file, true);
            std::fprintf(stderr, "misterplexd: LAN_AUTH secret=%s\n", ok ? "REDACTED" : "missing (refusing)");
        }
        if (misterplex::reportCodeEnabled()) {
            const misterplex::ReportPack pack = misterplex::prepareBootReport();
            if (pack.ok)
                std::fprintf(stderr, "misterplexd: report code=%s pack=%s\n", pack.code.c_str(),
                             pack.path.c_str());
            else
                std::fprintf(stderr, "misterplexd: report code=%s pack not written\n",
                             pack.code.c_str());
        }
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
        analogRasterConf = loadConf(confPath, "ANALOG_RASTER");
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
        markerFlags = misterplex::markers::loadFlags(
            {[&](const char* key) { return loadConf(confPath, key); },
             [](const char* key) { return std::getenv(key); }});
        std::fprintf(stderr,
                     "misterplexd: MARKERS=%d SKIP_INTRO=%s SKIP_CREDITS=%s UPNEXT=%d countdown=%d fallback=%d "
                     "passout=%d\n",
                     markerFlags.markers ? 1 : 0, misterplex::markers::modeName(markerFlags.skipIntro),
                     misterplex::markers::modeName(markerFlags.skipCredits), markerFlags.upNext ? 1 : 0,
                     markerFlags.countdownS, markerFlags.fallbackS, markerFlags.passoutMin);
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
        // GH#46: CRT title-safe + UI size (browse Y toggles UI_SIZE).
        misterplex::setUiSafePrefs(misterplex::parseUiSafeMarginPct(loadConf(confPath, "UI_SAFE_MARGIN")),
                                   misterplex::parseUiSize(loadConf(confPath, "UI_SIZE")));
        // E9: per-tube edges. Blank / absent stays the fixed title-safe above.
        {
            misterplex::OverscanEdges edges;
            edges.left = misterplex::parseOverscanEdgePct(loadConf(confPath, "OVERSCAN_LEFT"));
            edges.right = misterplex::parseOverscanEdgePct(loadConf(confPath, "OVERSCAN_RIGHT"));
            edges.top = misterplex::parseOverscanEdgePct(loadConf(confPath, "OVERSCAN_TOP"));
            edges.bottom = misterplex::parseOverscanEdgePct(loadConf(confPath, "OVERSCAN_BOTTOM"));
            misterplex::setOverscanEdges(edges);
            std::fprintf(stderr, "misterplexd: OVERSCAN_LEFT=%d OVERSCAN_RIGHT=%d OVERSCAN_TOP=%d OVERSCAN_BOTTOM=%d\n",
                         edges.left, edges.right, edges.top, edges.bottom);
        }
        v = loadConf(confPath, "SUBTITLE_LANG");
        if (!v.empty())
            misterplex::processSubtitleDefaults().lang = v;
        v = loadConf(confPath, "SUBTITLE_FORCED");
        if (!v.empty()) {
            const std::string f = v;
            misterplex::processSubtitleDefaults().forcedOnly =
                (f == "1" || f == "true" || f == "yes" || f == "on");
        }
        std::fprintf(stderr, "misterplexd: SUBTITLE_LANG=%s SUBTITLE_FORCED=%d\n",
                     misterplex::processSubtitleDefaults().lang.empty()
                         ? "off"
                         : misterplex::processSubtitleDefaults().lang.c_str(),
                     misterplex::processSubtitleDefaults().forcedOnly ? 1 : 0);
        // Soft overlay only. safe parks in the letterbox bar; picture keeps
        // the over-picture placement. Does not change PMS burn-in URLs.
        misterplex::setSubtitlePosition(
            misterplex::parseSubtitlePosition(loadConf(confPath, "SUBTITLE_POSITION")));
        std::fprintf(stderr, "misterplexd: SUBTITLE_POSITION=%s\n",
                     misterplex::subtitlePositionName(misterplex::subtitlePosition()));
        // Embedded text on the overlay. Default off: identical to this tip.
        // MPX_SOFT_SUBS wins over the conf key when the environment sets it.
        {
            std::string soft = loadConf(confPath, "SOFT_SUBS");
            if (const char* e = std::getenv("MPX_SOFT_SUBS")) {
                if (e[0])
                    soft = e;
            }
            misterplex::setSoftSubsEnabled(misterplex::parseSoftSubsFlag(soft));
            std::fprintf(stderr, "misterplexd: SOFT_SUBS=%d\n", misterplex::softSubsEnabled() ? 1 : 0);
        }
        // OpenSubtitles.com on-device fallback. Env wins over conf. Never log the key.
        {
            auto setIfUnset = [](const char* envKey, const std::string& val) {
                if (val.empty() || std::getenv(envKey))
                    return;
                setenv(envKey, val.c_str(), 0);
            };
            setIfUnset("OPENSUBTITLES_API_KEY", loadConf(confPath, "OPENSUBTITLES_API_KEY"));
            setIfUnset("MPX_OPENSUBTITLES_API_KEY", loadConf(confPath, "OPENSUBTITLES_API_KEY"));
            setIfUnset("OPENSUBTITLES_USERNAME", loadConf(confPath, "OPENSUBTITLES_USERNAME"));
            setIfUnset("OPENSUBTITLES_PASSWORD", loadConf(confPath, "OPENSUBTITLES_PASSWORD"));
            v = loadConf(confPath, "OPENSUBS_FALLBACK");
            if (!v.empty() && !std::getenv("MPX_OPENSUBS_FALLBACK") && !std::getenv("OPENSUBS_FALLBACK"))
                setenv("MPX_OPENSUBS_FALLBACK", v.c_str(), 0);
            std::fprintf(stderr, "misterplexd: OPENSUBS_FALLBACK key_present=%d\n",
                         misterplex::opensubs::keyPresent() ? 1 : 0);
        }
        // BURN_SUBS_FIX default off. Env MPX_BURN_SUBS_FIX or BURN_SUBS_FIX wins.
        // N6: a legacy conf BURN_SUBS_FIX=1 is an explicit opt-in (upgraded boxes
        // keep burning). Absent or 0 stays off. The example conf leaves it commented.
        v = loadConf(confPath, "BURN_SUBS_FIX");
        if (misterplex::burnSubsFixConfOptIn(v.c_str()) && !std::getenv("MPX_BURN_SUBS_FIX") &&
            !std::getenv("BURN_SUBS_FIX"))
            setenv("MPX_BURN_SUBS_FIX", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: BURN_SUBS_FIX=%d\n",
                     misterplex::burnSubsFixEnabled() ? 1 : 0);
        // AUTO_BURN default off (sec-cleanup-1101). Env MPX_AUTO_BURN or AUTO_BURN wins.
        v = loadConf(confPath, "AUTO_BURN");
        if (!v.empty() && !std::getenv("MPX_AUTO_BURN") && !std::getenv("AUTO_BURN"))
            setenv("MPX_AUTO_BURN", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: AUTO_BURN=%d\n", misterplex::autoBurnEnabled() ? 1 : 0);
        // HTTP_RANGE_RESUME default on. Env MPX_HTTP_RANGE_RESUME or
        // HTTP_RANGE_RESUME wins. Off: a direct Part stays on libav HTTP
        // (no custom AVIO Range resume).
        v = loadConf(confPath, "HTTP_RANGE_RESUME");
        if (!v.empty() && !std::getenv("MPX_HTTP_RANGE_RESUME") && !std::getenv("HTTP_RANGE_RESUME"))
            setenv("MPX_HTTP_RANGE_RESUME", v.c_str(), 0);
        std::fprintf(stderr, "misterplexd: HTTP_RANGE_RESUME=%d\n",
                     misterplex::httprange::rangeResumeEnabled() ? 1 : 0);
        // Phase 4: match-source-Hz / Content FPS (see docs/match-source-hz.md).
        // Conf is applied on each play: SOURCE_FPS selects Content FPS hint from PMS
        // metadata (or forces 12/24/30/60). MATCH_SOURCE_HZ=on still cannot switch
        // modelines without HPS switchres — logs target only.
        v = loadConf(confPath, "MATCH_SOURCE_HZ");
        if (!v.empty())
            matchSourceHz = v;
        v = loadConf(confPath, "AUTO_PICTURE_MODE");
        if (!v.empty())
            autoPictureConf = v;
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
    // Env wins when set. Only the exact word "advise" logs. Off stays silent.
    autoPictureMode = misterplex::autopic::resolveAutoPictureMode(
        std::getenv("AUTO_PICTURE_MODE"), autoPictureConf);
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
                            "MPX_STICK_I420", "MPX_FPGA_MODE", "NET_RESUME"}) {
        const auto value = loadConf(confPath, key);
        // Conf wins over stale shell env so mode-pair deploys are deterministic.
        if (!value.empty())
            ::setenv(key, value.c_str(), 1);
    }
    if (misterplex::netresume::enabledFromEnv(std::getenv("NET_RESUME")))
        std::fprintf(stderr, "misterplexd: NET_RESUME on\n");
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
        if (ep.menuSrc == "MPX_LAB_HOST" || ep.menuSrc == "conf MPX_MENU_URL")
            ::setenv("MPX_MENU_URL", ep.menu.c_str(), 1);
        if (ep.xcodeSrc == "MPX_LAB_HOST" || ep.xcodeSrc == "conf AIRPLAY_XCODE" ||
            ep.xcodeSrc == "conf GCAST_XCODE") {
            ::setenv("AIRPLAY_XCODE", ep.xcode.c_str(), 1);
            ::setenv("GCAST_XCODE", ep.xcode.c_str(), 1);
        }
        std::fprintf(stderr, "%s\n", le::logLine(ep).c_str());
    }
    // #275: the lab ladder helper is gone; MPX_LADDER_URL / *_LADDER_PROXY are ignored.
    if (std::getenv("MPX_LADDER_URL") || std::getenv("MPX_REMOTE_LADDER_PROXY") || std::getenv("REMOTE_LADDER_PROXY"))
        std::fprintf(stderr, "misterplexd: ladder env ignored (helper removed, #275); direct PMS\n");
    // DISPLAY/CONTENT 480i ⇒ present-only R15 bank (720×480 / 0x301FF000).
    if (!std::getenv("MPX_PRESENT_480I")) {
        const auto dlab = loadConf(confPath, "DISPLAY_RES");
        const auto clab = loadConf(confPath, "CONTENT_RES");
        if (dlab == "480i" || clab == "480i")
            ::setenv("MPX_PRESENT_480I", "1", 0);
    }
    misterplex::MediaPlayer player;
    const bool spelunkrOn = misterplex::envFlagWanted("SPELUNKR");
    if (spelunkrOn)
        player.setSpelunkrTv(true);
    player.setOverscanPersist([confPath](int left, int right, int top, int bottom) {
        const bool l = upsertConfKey(confPath, "OVERSCAN_LEFT", std::to_string(left));
        const bool r = upsertConfKey(confPath, "OVERSCAN_RIGHT", std::to_string(right));
        const bool t = upsertConfKey(confPath, "OVERSCAN_TOP", std::to_string(top));
        const bool b = upsertConfKey(confPath, "OVERSCAN_BOTTOM", std::to_string(bottom));
        return l && r && t && b;
    });
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
    // Product default: the on-device Plex TV browser. MPX_PLEXUI=0 opts out.
    // plexui is self-contained; imply the browse flags so plexUiEnabled stays true.
    const bool plexUiWanted = !spelunkrOn && misterplex::plexui::plexUiFlagOn();
    const bool plexUiOn = !spelunkrOn && misterplex::plexui::plexUiEnabled(
        browseV2Setting == misterplex::BrowseV2Setting::On || plexUiWanted,
        browseNavCommands || plexUiWanted);
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
    auto& ytWarm = misterplex::gcast::yt_warm::process();
    // P2-b: do not spawn the Android helper when YouTube casting is off.
    if (!spelunkrOn && misterplex::envFlagWanted("GYT") && !g_stop.load() && !g_termHeld.load() &&
        (misterplex::CoreGate::process().owenbOpen() || misterplex::CoreGate::process().open()))
        ytWarm.startAsync();
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
    {
        bool rasterOk = true;
        const auto rasterWant = misterplex::plexob::parseRasterWant(analogRasterConf, &rasterOk);
        if (!rasterOk)
            std::fprintf(stderr, "misterplexd: ANALOG_RASTER=%s ignored (use 480i, 240p or auto)\n",
                         analogRasterConf.c_str());
        else if (rasterWant != misterplex::plexob::RasterWant::Unset)
            std::fprintf(stderr, "misterplexd: ANALOG_RASTER=%s (%s)\n", misterplex::plexob::rasterWantName(rasterWant),
                         rasterWant == misterplex::plexob::RasterWant::Auto ? "same as unset: no action" : "240p core only");
        player.setAnalogRaster(rasterWant);
    }
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
    if (subtitleMode == "burn")
        player.setSubtitleBurnFallback(true);
    if (subtitleStreamId >= 0)
        player.setSubtitleStreamIndex(subtitleStreamId);
    {
        std::string trackPrefs = "/media/fat/misterplex/show_tracks.txt";
        if (const char* env = std::getenv("MPX_TRACK_PREFS"); env && *env)
            trackPrefs = env;
        else if (const auto slash = confPath.find_last_of('/'); slash != std::string::npos)
            trackPrefs = confPath.substr(0, slash + 1) + "show_tracks.txt";
        const misterplex::ShowTrackLoad loaded = misterplex::loadProcessShowTracks(trackPrefs);
        std::fprintf(stderr, "misterplexd: show tracks %s (%zu kept%s)\n", trackPrefs.c_str(), loaded.kept,
                     loaded.recovered ? ", corrupt file recovered" : "");
        std::string rememberPath = "/media/fat/misterplex/remember_tracks.txt";
        if (const auto slash = trackPrefs.find_last_of('/'); slash != std::string::npos)
            rememberPath = trackPrefs.substr(0, slash + 1) + "remember_tracks.txt";
        const misterplex::RememberTrackLoad remembered = misterplex::loadProcessRememberTracks(rememberPath);
        std::fprintf(stderr, "misterplexd: remember tracks %s (%zu kept%s)\n", rememberPath.c_str(), remembered.kept,
                     remembered.recovered ? ", corrupt file recovered" : "");
        std::string picturePath = "/media/fat/misterplex/remember_picture.txt";
        if (const auto slash = rememberPath.find_last_of('/'); slash != std::string::npos)
            picturePath = rememberPath.substr(0, slash + 1) + "remember_picture.txt";
        if (misterplex::rememberPictureEnabled()) {
            const misterplex::RememberPictureLoad pictured = misterplex::loadProcessRememberPicture(picturePath);
            std::fprintf(stderr, "misterplexd: remember picture %s (%zu kept%s)\n", picturePath.c_str(), pictured.kept,
                         pictured.recovered ? ", corrupt file recovered" : "");
        } else {
            std::fprintf(stderr, "misterplexd: remember picture off (REMEMBER_PICTURE=0); %s not loaded\n",
                         picturePath.c_str());
        }
        const bool trackServerSync = misterplex::trackPrefServerSyncEnabled(loadConf(confPath, "track_pref_server_sync"));
        player.setTrackPrefServerSync(trackServerSync);
        std::fprintf(stderr, "misterplexd: track_pref_server_sync=%d\n", trackServerSync ? 1 : 0);
    }
    {   // controller_track_shortcuts=<modifier>,<audio>,<subtitle>. Unset or unclear = off. Env wins.
        const char* envTk = std::getenv("CONTROLLER_TRACK_SHORTCUTS");
        const std::string confTk = loadConf(confPath, "controller_track_shortcuts");
        const std::string tk = (envTk && *envTk) ? std::string(envTk) : confTk;
        std::fprintf(stderr, "misterplexd: controller_track_shortcuts=%s\n",
                     player.setControllerTrackShortcuts(tk) ? "on" : "off");
    }
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
    // #328 (reviewer): initPresent() fails while the core gate is still closed at start (PlexOB
    // answers CAPS later). The server catalog and the PlexUI / browse v2 controller do not need a
    // present path, so they are set up either way; only the paint/idle/poll workers wait for one.
    // Gating them on initPresent left browseTargets empty and browseV2_ false: the legacy library_
    // page opened with no server and no fetch (rows=0).
    const bool presentOk = player.initPresent();
    if (!presentOk)
        std::fprintf(stderr, "misterplexd: WARNING no present path — companion only (browse still set up)\n");
    if (!spelunkrOn) {
        // MPX_BROWSE_V2=1 (lab, browse_v2.hpp): the browser's worker, fetch and paint path
        // must be in place before the first paint and the polls. Unset: nothing happens here.
        const auto browsePlayHook = [&](const misterplex::BrowsePlayTicket& t) -> uint64_t {
            std::function<uint64_t(const misterplex::BrowsePlayTicket&)> hook;
            {
                std::lock_guard<std::mutex> lk(browsePlayMu);
                hook = browsePlay;
            }
            return hook ? hook(t) : 0;
        };
        {
            static misterplex::PlexResourceCache resourceCache;
            misterplex::loadPlexResourceCache(resourceCache, plexTvToken, [&]() {
                return misterplex::fetchPlexTvResourcesJson(plexTvToken, 8);
            });
            std::string tokenConf;
            for (const auto& line : loadConfAll(confPath, "PLEX_SERVER_TOKENS")) {
                if (!tokenConf.empty())
                    tokenConf += ',';
                tokenConf += line;
            }
            std::string nameConf;
            for (const auto& line : loadConfAll(confPath, "PLEX_SERVER_NAMES")) {
                if (!nameConf.empty())
                    nameConf += ',';
                nameConf += line;
            }
            std::vector<std::string> bases = servers;
            if (bases.empty() && !defaultPms.empty())
                bases.push_back(defaultPms);
            browseTargets = misterplex::resolvePlexServerCatalog(
                bases, serverRefs, resourceCache.devices,
                misterplex::parseExplicitServerTokens(tokenConf), readConfToken(),
                misterplex::parseExplicitServerNames(nameConf));
            {   // #324/#329: only servers plex.tv marks owned=1 may take the burn subtitle part
                // PUT; keyed on their machine id (clientIdentifier), not host:port.
                std::vector<std::string> ownedIds;
                std::vector<std::string> ownedHosts;
                for (const auto& d : resourceCache.devices) {
                    if (!d.owned)
                        continue;
                    ownedIds.push_back(!d.clientIdentifier.empty() ? d.clientIdentifier
                                                                   : d.machineIdentifier);
                    for (const auto& c : d.connections) {
                        if (!c.uri.empty())
                            ownedHosts.push_back(c.uri);
                        if (!c.address.empty() && !c.port.empty())
                            ownedHosts.push_back(c.address + ":" + c.port);
                    }
                }
                if (!ownedIds.empty() || !ownedHosts.empty()) {
                    misterplex::burnPutOwners().set(ownedIds, ownedHosts);
                    std::fprintf(stderr, "misterplexd: burn PUT owned servers=%zu\n", ownedIds.size());
                } else {
                    std::fprintf(stderr, "misterplexd: burn PUT catalog empty (keep linked owners)\n");
                }
            }
            std::string names;
            for (const auto& t : browseTargets) {
                if (!names.empty())
                    names += ',';
                names += t.name;
            }
            std::fprintf(stderr, "misterplexd: plex servers %zu [%s] resources=%zu\n",
                         browseTargets.size(), names.c_str(), resourceCache.devices.size());
        }
        if (plexUiOn) {
            misterplex::plexui::PlexUiController::Config pc;
            std::string account = misterplex::plexui::plexUiAccountToken(plexTvToken);
            if (account.empty())
                account = readConfToken();
            pc.accountToken = std::move(account);
            pc.fontDir = misterplex::plexui::plexUiDirFromEnvironment();
            pc.clientId = machineId.empty() ? "misterplex" : machineId;
            if (!browseTargets.empty()) {
                for (const auto& t : browseTargets) {
                    if (t.baseUrl.empty())
                        continue;
                    auto s = misterplex::plexui::makeLocalPlexServer(t.name, t.baseUrl, t.token);
                    if (!t.machineId.empty())
                        s.machineIdentifier = t.machineId;
                    if (!t.clientId.empty())
                        s.clientIdentifier = t.clientId;
                    s.owned = t.owned;
                    pc.seedServers.push_back(std::move(s));
                }
            } else {
                const std::string seedTok = readConfToken();
                for (const auto& url : servers) {
                    if (url.empty())
                        continue;
                    pc.seedServers.push_back(
                        misterplex::plexui::makeLocalPlexServer({}, url, seedTok));
                }
            }
            pc.discoverLocal = [] { return misterplex::plexui::discoverLocalPlexServers(600); };
            pc.persistAccountToken = [&](const std::string& tok) {
                if (!misterplex::plexui::plexAccountTokenOk(tok))
                    return false;
                // The plex.tv account token is not the PMS PLEX_TOKEN. Keep them apart
                // unless PLEX_TOKEN is still empty.
                if (!upsertConfKey(confPath, "PLEXTV_TOKEN", tok)) {
                    std::fprintf(stderr, "misterplexd: failed to persist PLEXTV_TOKEN\n");
                    return false;
                }
                if (readConfToken().empty()) {
                    if (!upsertConfKey(confPath, "PLEX_TOKEN", tok)) {
                        std::fprintf(stderr, "misterplexd: failed to persist PLEX_TOKEN\n");
                        return false;
                    }
                    writeConfToken(tok);
                    std::fprintf(stderr,
                                 "misterplexd: PLEX_TOKEN was empty; stored the plex.tv account token\n");
                }
                return true;
            };
            pc.persistUiSize = [&](const char* name) {
                if (!name || !*name)
                    return false;
                if (!upsertConfKey(confPath, "UI_SIZE", name)) {
                    std::fprintf(stderr, "misterplexd: failed to persist UI_SIZE\n");
                    return false;
                }
                return true;
            };
            {
                std::string homeId = loadConf(confPath, "PLEX_HOME_ID");
                std::string homeTitle = loadConf(confPath, "PLEX_HOME_TITLE");
                std::string homeTok = loadConf(confPath, "PLEX_HOME_TOKEN");
                if (!misterplex::plexui::plexAccountTokenOk(homeTok) ||
                    !misterplex::plexui::homeIdOk(homeId)) {
                    homeId.clear();
                    homeTitle.clear();
                    homeTok.clear();
                } else {
                    homeTitle = misterplex::plexui::homeTitleForConf(homeTitle);
                }
                pc.homeUserId = std::move(homeId);
                pc.homeUserTitle = std::move(homeTitle);
                pc.homeUserToken = std::move(homeTok);
            }
            pc.persistHomeSession = [&](const misterplex::plexui::HomeSession& s) {
                if (s.token.empty()) {
                    const bool idOk = upsertConfKey(confPath, "PLEX_HOME_ID", "");
                    const bool titleOk = upsertConfKey(confPath, "PLEX_HOME_TITLE", "");
                    const bool tokOk = upsertConfKey(confPath, "PLEX_HOME_TOKEN", "");
                    return idOk && titleOk && tokOk;
                }
                if (!misterplex::plexui::plexAccountTokenOk(s.token) ||
                    !misterplex::plexui::homeIdOk(s.id))
                    return false;
                const std::string title = misterplex::plexui::homeTitleForConf(s.title);
                const bool idOk = upsertConfKey(confPath, "PLEX_HOME_ID", s.id);
                const bool titleOk = upsertConfKey(confPath, "PLEX_HOME_TITLE", title);
                const bool tokOk = upsertConfKey(confPath, "PLEX_HOME_TOKEN", s.token);
                return idOk && titleOk && tokOk;
            };
            player.enablePlexUi(pc, browsePlayHook);
            std::fprintf(stderr, "misterplexd: plexui token=%s fonts=%s seeds=%zu UI_SAFE_MARGIN=%d UI_SIZE=%s home=%s\n",
                         pc.accountToken.empty() ? "(none)" : "(set)",
                         pc.fontDir.c_str(), pc.seedServers.size(),
                         misterplex::uiSafePrefs().marginPct,
                         misterplex::uiSizeName(misterplex::uiSafePrefs().size),
                         pc.homeUserToken.empty() ? "none" : "set");
        } else if (browseV2Setting == misterplex::BrowseV2Setting::On) {
            misterplex::BrowseController::Config bc;
            bc.menuUrl = browseMenuUrl;
            bc.servers = browseTargets;
            if (!browseTargets.empty()) {
                bc.pmsBase = browseTargets.front().baseUrl;
                bc.pmsToken = browseTargets.front().token;
            } else {
                bc.pmsBase = defaultPms;
                bc.pmsToken = readConfToken();
            }
            const auto bg = misterplex::ddrFrameGeometryForPresentedSize(player.decodeW(),
                                                                       player.decodeH());
            bc.cols = misterplex::browseTitleCols(bg.coded_width);
            bc.rows = misterplex::libraryRowsOnScreen(bg.coded_width, bg.coded_height);
            player.enableBrowseV2(bc, browsePlayHook, browseNavCommands);
            std::fprintf(stderr, "misterplexd: browse v2 helper=%s cols=%d rows=%d pms=%s\n",
                         misterplex::logUrl(bc.menuUrl).c_str(), bc.cols, bc.rows,
                         defaultPms.empty() ? "(none)" : defaultPms.c_str());
        }
    }
    // #342: the workers start once a present path exists: at boot, or later from the main loop.
    const auto startPresentWorkers = [&]() {
        if (!spelunkrOn && misterplex::librarylist::browseLibListEnabled(std::getenv(misterplex::librarylist::kBrowseLibListEnv))) {
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
    };
    std::string lastPlayedChannel = spelunkrOn ? loadConf(confPath, "SPELUNKR_LAST_CHANNEL") : "";
    if (spelunkrOn) {
        player.setSpelunkrTv(true);
        // Core loads are manual-only (MPX_CORE_MANUAL_ONLY): the daemon must stay up
        // with the core unloaded. Each channel play re-runs prepareSpelunkrVideo().
        // GBE #624 P2-1: core already loaded / loading during the 5 s wait must not show the landing before the lineup
        if (!lastPlayedChannel.empty())
            player.beginSpelunkrTune(0, "Spelunkr", "");
        if (!player.prepareSpelunkrVideo())
            std::fprintf(stderr, "misterplexd: spelunkr: video core not ready at start; "
                                 "deferring video prep to first channel play\n");
    }
    if (presentOk)
        startPresentWorkers();
    // #342: present failed at boot -> bounded backoff re-tries from the main loop tick.
    misterplex::PresentRetry presentRetry(!presentOk, std::chrono::steady_clock::now());

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
        // A lab play-file plays its own file: never a transcode (FPGA Bug 19:37 N1).
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

    // SPELUNKR=1: TV mode on the loaded owenb MisterPlex core. Pad Up/Down zap, Info opens
    // the guide, banner/guide draw on ovl_plane. Lives until process exit.
    std::unique_ptr<misterplex::spelunkr::Session> tvSess;
    std::recursive_mutex tvMu;   // pad + companion HTTP; not held across HTTPS
    std::atomic<uint64_t> spelunkrEofGen{0};  // play-thread EOF handoff; main loop consumes
    std::atomic<uint64_t> spelunkrPlayGen{0};  // GBE N-4: tune gen known to play job
    std::atomic<int> spelunkrPlayItemIndex{-1};  // GBE N-2: item index that is playing
    std::atomic<int64_t> spelunkrPlayOffsetMs{-1};  // start offset of the item that is playing
    std::atomic<int> spelunkrEofItemIndex{-1};  // GBE N-2: ended item carried with EOF
    std::atomic<int64_t> spelunkrEofOffsetMs{-1};  // start offset of the ended play
    std::atomic<int64_t> spelunkrEofPlayedMs{-1};  // EOF progress; length for from-zero plays
    std::atomic<unsigned> spelunkrPlayFailedSkips{0};  // P2 11-10: failedSkips of the play job that is playing
    misterplex::spelunkr::FailedItemHandoff spelunkrFailedSkip;  // P2 11-10: skip-with-banner handoff
    misterplex::spelunkr::FirstPictureWatchdog spelunkrFirstPicture;  // under tvMu
    misterplex::spelunkr::SkipBudgetReset spelunkrBudgetReset;        // under tvMu (GBE #621 P2-a/P3-a)
    bool tvHadCoreOpenOnce = false;  // GBE N-3 early: set only after a real tune posts
    bool tvStopping = false;    // under tvMu; rejects commands copied before callback removal
    std::thread guideThr;
    int64_t lastZapMs = 0;       // under tvMu; steady clock (GH#411)
    misterplex::spelunkr::ArchiveCache archiveCache;
    auto spelunkrNowMs = []() -> int64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
    auto zapSteadyMs = []() -> int64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    auto fetchBroadcast = [](const std::string& rel, std::string& body, int& status,
                             const std::function<bool()>& cancelled) -> bool {
            misterplex::TlsVerifiedRequest q;
            q.url = std::string(misterplex::spelunkr::kBroadcastBase) + rel;
            q.maxBody = 2u << 20;
            q.timeoutSec = 8;
            q.cancelled = cancelled;
            const misterplex::TlsVerifiedResult r = misterplex::tlsVerifiedGet(q);
            status = r.status;
            body = r.body;
            return r.transportOk && r.status == 200 && !r.cancelled;
        };
        auto ensureManifest = [&](int idx, const std::function<bool()>& cancelled) -> bool {
            std::string rel;
            {
                std::lock_guard<std::recursive_mutex> lock(tvMu);
                if (!tvSess || idx < 0 || idx >= static_cast<int>(tvSess->lineup.channels.size()))
                    return false;
                if (tvSess->hasManifest(idx))
                    return true;
                const misterplex::spelunkr::VersionPtr* ver =
                    misterplex::spelunkr::currentVersion(tvSess->lineup.channels[static_cast<size_t>(idx)]);
                if (!ver)
                    return false;
                rel = ver->manifest;
            }
            std::string body;
            int st = 0;
            if (!fetchBroadcast(rel, body, st, cancelled)) {
                std::fprintf(stderr, "misterplexd: SPELUNKR manifest failed status=%d idx=%d\n", st, idx);
                return false;
            }
            misterplex::spelunkr::Manifest man;
            if (!misterplex::spelunkr::parseManifest(body, man))
                return false;
            std::lock_guard<std::recursive_mutex> lock(tvMu);
            return tvSess && tvSess->setManifest(idx, std::move(man));
        };
        auto publishGuide = [&]() {
            std::lock_guard<std::recursive_mutex> lock(tvMu);
            if (!tvSess)
                return;
            std::vector<std::string> lines;
            for (const auto& g : tvSess->guideRows(spelunkrNowMs())) {
                std::string line = std::to_string(g.dial) + "  " + g.name;
                if (!g.nowTitle.empty())
                    line += "  " + g.nowTitle;
                lines.push_back(std::move(line));
            }
            player.setSpelunkrGuideOverlay(tvSess->guideOpen, tvSess->guideCursor, std::move(lines));
        };
        auto archiveHttpHeaders = []() -> std::string {
            return std::string("User-Agent: ") + misterplex::spelunkr::kArchiveUserAgent + "\r\n";
        };
        auto archiveGet = [](const std::string& url, size_t maxBody, bool range,
                             const std::function<bool()>& cancelled) -> misterplex::spelunkr::ArchiveHttpGet {
            misterplex::TlsVerifiedRequest q;
            q.url = url;
            q.maxBody = maxBody;
            q.timeoutSec = 8;
            q.cancelled = cancelled;
            q.headers = {{"User-Agent", misterplex::spelunkr::kArchiveUserAgent}};
            if (range)
                q.headers.emplace_back("Range", "bytes=0-511");
            const misterplex::TlsVerifiedResult r = misterplex::tlsVerifiedGet(q);
            misterplex::spelunkr::ArchiveHttpGet o;
            o.transportOk = r.transportOk && !r.cancelled;
            o.status = r.status;
            o.body = r.body;
            o.err = r.cancelled ? "cancelled" : r.err;
            return o;
        };
    misterplex::spelunkr::PlayQueue playQ;
    if (misterplex::envFlagWanted("SPELUNKR")) {
        // Arm before lineup GET so pad Up/Down cannot open Plex browse (GH#411).
        player.setSpelunkrTv(true);
        playQ.start([&](misterplex::spelunkr::PlayRequest req, const std::function<bool()>& superseded) {
            const uint64_t gen = req.generation;
            auto tvCurrent = [&]() -> bool {
                std::lock_guard<std::recursive_mutex> lock(tvMu);
                return tvSess && tvSess->tv.current(gen);
            };
            auto fail = [&](const std::string& detail) {
                std::lock_guard<std::recursive_mutex> lock(tvMu);
                if (!superseded() && tvCurrent()) {
                    if (req.firstPictureAttempt != 0)
                        std::fprintf(stderr, "misterplexd: spelunkr: first-picture watchdog: landing: %s\n",
                                     detail.c_str());
                    player.showSpelunkrError(detail, req.advanceItem);
                }
            };
            if (req.firstPictureAttempt != 0) {
                if (superseded() || !tvCurrent())
                    return;
                // Stop audio too, but retain the sticky frame / pending banner.
                // Never join the play callbacks while holding tvMu.
                player.stop();
                if (superseded() || !tvCurrent())
                    return;
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            bool timedOut = false;
            // P2 11-10: the item this advance is opening; an overrun skips it with a banner.
            std::atomic<int> tryingItem{req.endedItemIndex};
            auto postFailedItemSkip = [&](int failedItem, unsigned priorSkips, const char* why) {
                if (failedItem < 0)
                    return false;
                spelunkrFailedSkip.post(gen, failedItem, priorSkips, why);
                return true;
            };
            const std::function<bool()> cancelled = [&] {
                if (superseded() || !tvCurrent())
                    return true;
                if (req.advanceItem && std::chrono::steady_clock::now() >= deadline) {
                    if (!timedOut) {
                        timedOut = true;
                        std::fprintf(stderr, "misterplexd: spelunkr: advance timeout item=%d skips=%u gen=%llu\n",
                                     tryingItem.load(), req.failedSkips, static_cast<unsigned long long>(gen));
                        if (!postFailedItemSkip(tryingItem.load(), misterplex::spelunkr::sharedSkipBudget(req), "advance timeout"))
                            fail("PREFETCH TIMEOUT");
                    }
                    return true;
                }
                return false;
            };
            int idx = -1;
            size_t lineupN = 0;
            {
                std::lock_guard<std::recursive_mutex> lock(tvMu);
                if (!tvSess || cancelled() || !tvSess->tv.current(gen))
                    return;
                idx = misterplex::spelunkr::resolvePlayIndex(tvSess->lineup, req);
                if (idx < 0) {
                    fail("BAD CHANNEL");
                    return;
                }
                lineupN = tvSess->lineup.channels.size();
            }
            std::fprintf(stderr, "misterplexd: spelunkr: playjob gen=%llu idx=%d id=%s dial=%d lineup=%zu\n",
                         static_cast<unsigned long long>(gen), idx, req.id.c_str(), req.dial, lineupN);
            if (!ensureManifest(idx, cancelled)) {
                if (!cancelled() && tvCurrent())
                    fail("MANIFEST FAILED");
                return;
            }
            if (cancelled() || !tvCurrent())
                return;
            misterplex::spelunkr::NowPlaying np0;
            {
                std::lock_guard<std::recursive_mutex> lock(tvMu);
                if (!tvSess || cancelled() || !tvSess->tv.current(gen))
                    return;
                np0 = misterplex::spelunkr::nowPlayingOrFirst(
                    tvSess->lineup.channels[static_cast<size_t>(idx)], tvSess->mans[static_cast<size_t>(idx)],
                    spelunkrNowMs());
            }
            if (!np0.ok) {
                if (tvCurrent())
                    fail(req.advanceItem ? "NO NEXT" : "NO PROGRAMME");
                return;
            }
            misterplex::spelunkr::NowPlaying np = np0;
            if (req.advanceItem) {
                std::lock_guard<std::recursive_mutex> lock(tvMu);
                if (cancelled())
                    return;
                if (req.firstPictureAttempt != 0)
                    np = misterplex::spelunkr::nextAfterFirstPictureTimeout(
                        tvSess->lineup.channels[static_cast<size_t>(idx)],
                        tvSess->mans[static_cast<size_t>(idx)], req.endedItemIndex,
                        req.firstPictureAttempt, spelunkrNowMs());
                else if (req.failedSkips != 0)
                    np = misterplex::spelunkr::nextAfterFailedItem(
                        tvSess->lineup.channels[static_cast<size_t>(idx)],
                        tvSess->mans[static_cast<size_t>(idx)], req.endedItemIndex, req.failedSkips,
                        spelunkrNowMs());
                else
                    np = misterplex::spelunkr::nextAfterEof(
                        tvSess->lineup.channels[static_cast<size_t>(idx)],
                        tvSess->mans[static_cast<size_t>(idx)], np0, req.endedItemIndex, spelunkrNowMs(),
                        req.endedOffsetMs, req.endedPlayedMs);
                if (!np.ok) {
                    fail(req.firstPictureAttempt != 0 ? "FIRST PICTURE FAILED: NO NEXT"
                         : req.failedSkips != 0       ? "SKIP FAILED: NO NEXT"
                                                      : "NO NEXT");
                    return;
                }
                tryingItem.store(np.itemIndex);
                if (tvCurrent())
                    player.fillSpelunkrBannerTitle(np.title, np.channelName);
            }
            auto get = [&](const std::string& url, size_t maxBody, bool range) {
                return archiveGet(url, maxBody, range, cancelled);
            };
            int itemCount = 0;
            {
                std::lock_guard<std::recursive_mutex> lock(tvMu);
                if (tvSess && idx >= 0 && idx < static_cast<int>(tvSess->mans.size()))
                    itemCount = static_cast<int>(tvSess->mans[static_cast<size_t>(idx)].items.size());
            }
            int skips = 0;
            auto skipDead = [&](const misterplex::spelunkr::NowPlaying& cur) {
                misterplex::spelunkr::NowPlaying next;
                std::lock_guard<std::recursive_mutex> lock(tvMu);
                if (!tvSess || cancelled() || !tvSess->tv.current(gen) || idx < 0 ||
                    idx >= static_cast<int>(tvSess->lineup.channels.size()))
                    return next;
                return misterplex::spelunkr::skipToNextItem(
                    tvSess->lineup.channels[static_cast<size_t>(idx)],
                    tvSess->mans[static_cast<size_t>(idx)], cur, spelunkrNowMs());
            };
            auto takeSkip = [&](misterplex::spelunkr::NowPlaying next, const char* why, bool drawTitle = true) {
                if (!next.ok || next.itemIndex == np.itemIndex ||
                    (req.advanceItem && next.itemIndex == req.endedItemIndex) ||
                    misterplex::spelunkr::skipBudgetExhausted(++skips, itemCount))
                    return false;
                std::fprintf(stderr, "misterplexd: spelunkr: skip %s title=%s -> %s\n", why,
                             np.title.c_str(), next.title.c_str());
                np = std::move(next);
                tryingItem.store(np.itemIndex);
                if (drawTitle && tvCurrent())
                    player.fillSpelunkrBannerTitle(np.title, np.channelName);
                return true;
            };
            // P2: the header cache is cross-process/persistent (SD), so "warm" is a disk lookup,
            // not play-loop state. Shared across retunes in this loop; HeaderCacheStore is
            // internally locked, so sharing one instance across threads is safe.
            misterplex::spelunkr::HeaderCacheStore headerCacheStore;
            auto headerCacheWarm = [&](const std::string& url, int64_t size, const std::string& etag) {
                return headerCacheStore.hasEntry({url, size, etag});
            };
            for (;;) {
                int64_t itemSize = -1;
                std::string itemEtag;
                const auto run = misterplex::spelunkr::playJobDeadRun(
                    tvMu, [&] { return !cancelled() && tvCurrent(); },
                    [&] {
                        return misterplex::spelunkr::resolveArchivePlay(archiveCache, np.archiveId, np.file, get,
                                                                        cancelled, headerCacheWarm);
                    },
                    [&](const char* why) { return takeSkip(skipDead(np), why, false); }, player, np);
                if (run.skipped > 0)
                    std::fprintf(stderr, "misterplexd: spelunkr: dead run crossed n=%d unsupported=%d -> %s\n",
                                 run.skipped, run.unsupported, np.title.c_str());
                if (cancelled() || !tvCurrent())
                    return;
                if (run.resolved.kind == misterplex::spelunkr::ArchiveResolveKind::Cancelled)
                    return;
                if (!run.landed) {
                    std::fprintf(stderr, "misterplexd: SPELUNKR %s title=%s url=%s\n",
                                 misterplex::spelunkr::deadRunEndLog(run.resolved.kind), np.title.c_str(),
                                 misterplex::redactUrlTokens(run.resolved.url).c_str());
                    fail(misterplex::spelunkr::deadRunEndMessage(run, np.title));
                    return;
                }
                np.url = run.resolved.url;
                itemSize = run.resolved.size;
                itemEtag = run.resolved.etag;
            if (cancelled() || !tvCurrent())
                return;
            std::fprintf(stderr, "misterplexd: spelunkr: archive item host=%s cache=%zu\n",
                         misterplex::logUrl(misterplex::spelunkr::urlHost(np.url)).c_str(), archiveCache.size());
            if (!player.prepareSpelunkrVideo()) {
                fail("VIDEO PREPARE FAILED");
                return;
            }
            if (cancelled() || !tvCurrent())
                return;
            std::string prefetchFail;
            bool prefetchTimedOut = false;
            // The same libav open probes format/aspect and retains its IDR.
            // No ring or display settings change while the old session plays.
            auto prefetched = player.prefetchSpelunkr(
                np.url, np.offsetMs, archiveHttpHeaders(),
                [&] { return cancelled() || !tvCurrent(); }, prefetchFail, prefetchTimedOut,
                itemSize, itemEtag);
            if (cancelled() || !tvCurrent())
                return;
            if (!prefetched && prefetchFail.rfind("unsupported resolution", 0) == 0) {
                std::lock_guard<std::recursive_mutex> lock(tvMu);
                if (cancelled() || !tvCurrent())
                    return;
                std::fprintf(stderr, "misterplexd: spelunkr: caps rejected before cutover: %s title=%s\n",
                             prefetchFail.c_str(), np.title.c_str());
                // No realtime ARM video encoder path is available in the demux source.
                // Skip off-ring, retaining the old picture and the destination banner.
                const bool skipped = takeSkip(skipDead(np), "unsupported resolution");
                player.showSpelunkrBanner("CH " + std::to_string(np.dial), np.channelName,
                                         "unsupported resolution", true);
                if (skipped)
                    continue;
                fail("unsupported resolution: NO NEXT");
                return;
            }
            // P3: a 15 s dead-air prefetch timeout must not hard-fail the channel when a next
            // item exists -- skipDead()/takeSkip() first, mirroring "unsupported resolution"
            // above. shouldSkipOnPrefetchTimeout keeps that ordering (timeout only) testable
            // without the play loop's threads/locks/player. Any other prefetch failure falls
            // through unchanged to the fail() below.
            if (!prefetched && misterplex::spelunkr::shouldSkipOnPrefetchTimeout(prefetchTimedOut)) {
                std::lock_guard<std::recursive_mutex> lock(tvMu);
                if (cancelled() || !tvCurrent())
                    return;
                std::fprintf(stderr, "misterplexd: spelunkr: prefetch timeout before cutover: title=%s\n",
                             np.title.c_str());
                const bool skipped = takeSkip(skipDead(np), "prefetch timeout");
                player.showSpelunkrBanner("CH " + std::to_string(np.dial), np.channelName,
                                         "prefetch timeout", true);
                if (skipped)
                    continue;
                fail("PREFETCH TIMEOUT: NO NEXT");
                return;
            }
            if (!prefetched) {
                std::lock_guard<std::recursive_mutex> lock(tvMu);
                if (!cancelled() && tvCurrent())
                    fail(prefetchTimedOut ? "PREFETCH TIMEOUT" : "PREFETCH FAILED " + prefetchFail);
                return;
            }
            const int srcW = prefetched->format.width;
            const int srcH = prefetched->format.height;
            const auto srcDar = misterplex::reduceSourceAspect(prefetched->darNum, prefetched->darDen);
            const auto aspect = misterplex::spelunkr::p1SourceAspect(srcDar, srcW, srcH);
            std::fprintf(stderr,
                         "misterplexd: spelunkr: tune dial=%d id=%s title=%s off_ms=%lld url=%s "
                         "aspect=%u:%u gen=%llu note=%s\n",
                         np.dial, np.channelId.c_str(), np.title.c_str(),
                         static_cast<long long>(np.offsetMs), misterplex::redactUrlTokens(np.url).c_str(),
                         aspect.x, aspect.y, static_cast<unsigned long long>(gen), np.note.c_str());
            if (cancelled() || !tvCurrent())
                return;
            player.fillSpelunkrBannerTitle(np.title, np.channelName);
            // Only a probed, core-compatible source may replace the old picture.
            // Join callbacks under the OLD play gen, outside tvMu:
            // neither the demux nor its ended handler may wait on it.
            player.stop();
            misterplex::gate::GatePlayContext gateCtx;
            bool played = false;
            {
                std::lock_guard<std::recursive_mutex> lock(tvMu);
                if (cancelled() || !tvCurrent())
                    return;
                if (srcW > 0 && srcH > 0) {
                    player.setDecodeSize(srcW, srcH);
                    player.setSourceMediaSize(srcW, srcH);
                }
                player.setSourceAspect(aspect);
                player.setSourceTranscoded(false);
                player.setPlaybackTitle(std::to_string(np.dial) + " " + np.channelName + " " + np.title);
                std::fprintf(stderr,
                             "misterplexd: spelunkr: retain user OSD Video Output; no Progressive poke; "
                             "Aspect/Analog Aspect defaults only on Auto\n");
                spelunkrPlayItemIndex.store(np.itemIndex, std::memory_order_release);
                spelunkrPlayOffsetMs.store(misterplex::spelunkr::stampedPlayOffsetMs(np),
                                           std::memory_order_release);
                spelunkrPlayFailedSkips.store(misterplex::spelunkr::sharedSkipBudget(req), std::memory_order_release);
                spelunkrPlayGen.store(gen, std::memory_order_release);
                played = player.play(np.url, np.offsetMs, archiveHttpHeaders(), np.durationMs, gen, gateCtx,
                                     std::move(prefetched));
                if (played)
                    spelunkrFirstPicture.arm(gen, zapSteadyMs(), misterplex::spelunkr::sharedSkipBudget(req));
            }
            if (played)
                return;
            std::fprintf(stderr, "misterplexd: SPELUNKR play failed title=%s\n", np.title.c_str());
            if (!takeSkip(skipDead(np), "play fail")) {
                if (tvCurrent())
                    fail("PLAY FAILED " + np.title);
                return;
            }
            }
        });
        std::string chId = "primetime";
        if (const char* e = std::getenv("SPELUNKR_CHANNEL"); e && *e)
            chId = e;
        std::string body;
        int st = 0;
        misterplex::spelunkr::Lineup lineup;
        if (!fetchBroadcast("lineup.json", body, st, {})) {
            std::fprintf(stderr, "misterplexd: SPELUNKR lineup failed status=%d\n", st);
            player.showSpelunkrError("LINEUP FAILED");
        } else if (!misterplex::spelunkr::parseLineup(body, lineup)) {
            std::fprintf(stderr, "misterplexd: SPELUNKR lineup parse failed\n");
            player.showSpelunkrError("LINEUP INVALID");
        } else {
            tvSess = std::make_unique<misterplex::spelunkr::Session>();
            tvSess->setLineup(std::move(lineup));
            int idx = misterplex::spelunkr::channelIndexById(tvSess->lineup, chId);
            if (idx < 0)
                idx = 0;
            std::fprintf(stderr, "misterplexd: spelunkr: lineup n=%zu default idx=%d id=%s dial=%d\n",
                         tvSess->lineup.channels.size(), idx,
                         tvSess->lineup.channels[static_cast<size_t>(idx)].id.c_str(),
                         tvSess->lineup.channels[static_cast<size_t>(idx)].dial);
            // Pin the dial before pad Up/Down is live. idx_ defaults to -1 so
            // channelDelta cannot walk from channel 0 onto primetime (CH 8).
            tvSess->selectIndex(idx);
            tvSess->guideOpen = false;
            player.setSpelunkrTv(true);
            player.setSpelunkrCmd([&](misterplex::PlaybackCommand c) {
                std::lock_guard<std::recursive_mutex> lock(tvMu);
                if (tvStopping || !tvSess)
                    return;
                if (c == misterplex::PlaybackCommand::Stop) {
                  playQ.cancel();
                  tvSess->tv.bumpGeneration();
                  tvSess->guideOpen = false;
                  player.stop();
                  player.showSpelunkrLanding();
                  publishGuide();
                  return;
                }
                if (c == misterplex::PlaybackCommand::GuideToggle) {
                    tvSess->toggleGuide();
                    publishGuide();
                    return;
                }
                if (tvSess->guideOpen && (c == misterplex::PlaybackCommand::ChannelUp ||
                                          c == misterplex::PlaybackCommand::ChannelDown)) {
                    const int delta = (c == misterplex::PlaybackCommand::ChannelDown) ? 1 : -1;
                    int from = tvSess->guideCursor;
                    if (from < 0)
                        from = tvSess->tv.index();
                    const int cur = misterplex::spelunkr::channelDelta(tvSess->lineup, from, delta);
                    if (cur >= 0) {
                        tvSess->guideCursor = cur;
                        publishGuide();
                    }
                    return;
                }
                if (c == misterplex::PlaybackCommand::GuideTune) {
                    const int gidx = tvSess->guideCursor;
                    const auto t = tvSess->selectIndex(gidx);
                    tvSess->guideOpen = false;
                    publishGuide();
                    if (t.generation == 0)
                        return;
                    {
                        const int to = tvSess->tv.index();
                        misterplex::spelunkr::PlayRequest r =
                            misterplex::spelunkr::playRequestFor(tvSess->lineup, t.generation, to);
                        player.beginSpelunkrTune(r.generation, "CH " + std::to_string(r.dial),
                                                 (to >= 0 && to < static_cast<int>(tvSess->lineup.channels.size()))
                                                     ? tvSess->lineup.channels[static_cast<size_t>(to)].name
                                                     : "");
                        playQ.post(std::move(r));
                        tvHadCoreOpenOnce = true;  // GBE N-3 guide
                    }
                    return;
                }
                if (c != misterplex::PlaybackCommand::ChannelUp &&
                    c != misterplex::PlaybackCommand::ChannelDown)
                    return;
                // ChannelUp = next higher dial (lineup order). Guide list uses the
                // opposite sign so pad Up moves the highlight up the list.
                if (!misterplex::spelunkr::acceptZap(zapSteadyMs(), lastZapMs))
                    return;
                const int from = tvSess->tv.index();
                const int delta = (c == misterplex::PlaybackCommand::ChannelUp) ? 1 : -1;
                const auto t = tvSess->zapSelect(delta);
                if (t.generation == 0) {
                    // GH#411: keep debounce; do not let a failed zap retry at 120 ms
                    // and snap onto CH 8. Log the drop so glass is not silent.
                    std::fprintf(stderr,
                                 "misterplexd: spelunkr: zap dropped from=%d n=%zu\n", from,
                                 tvSess->lineup.channels.size());
                    return;
                }
                const int to = tvSess->tv.index();
                misterplex::spelunkr::PlayRequest r =
                    misterplex::spelunkr::playRequestFor(tvSess->lineup, t.generation, to);
                const int fromDial =
                    (from >= 0 && from < static_cast<int>(tvSess->lineup.channels.size()))
                        ? tvSess->lineup.channels[static_cast<size_t>(from)].dial
                        : -1;
                std::fprintf(stderr,
                             "misterplexd: spelunkr: zap %s idx=%d dial=%d -> idx=%d id=%s dial=%d gen=%llu n=%zu\n",
                             c == misterplex::PlaybackCommand::ChannelUp ? "up" : "down", from, fromDial, to,
                             r.id.c_str(), r.dial, static_cast<unsigned long long>(t.generation),
                             tvSess->lineup.channels.size());
                // Keep the destination banner over live video until the flip's first picture.
                player.beginSpelunkrTune(r.generation, "CH " + std::to_string(r.dial),
                                         (to >= 0 && to < static_cast<int>(tvSess->lineup.channels.size()))
                                             ? tvSess->lineup.channels[static_cast<size_t>(to)].name
                                             : "");
                playQ.post(std::move(r));
                tvHadCoreOpenOnce = true;  // GBE N-3 zap
            });
            publishGuide();
            guideThr = std::thread([&] {
                auto cancelled = [&] {
                    std::lock_guard<std::recursive_mutex> lock(tvMu);
                    return tvStopping || g_stop.load();
                };
                const int count = static_cast<int>(tvSess->lineup.channels.size());
                for (int i = 0; i < count && !cancelled(); ++i) {
                    if (!ensureManifest(i, cancelled) && !cancelled())
                        std::fprintf(stderr, "misterplexd: spelunkr guide channel %d unavailable\n", i);
                    if (!cancelled())
                        publishGuide();
                }
            });
        }
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
    player.setObIdleName(spelunkrOn ? "Spelunkr" : name);
    player.owenbFbconAtStart();   // OWENB-P1B 0009: attach a console a dead daemon left detached (G15)
    comp.setMachineId(machineId);
    comp.setPort(static_cast<uint16_t>(port));
    comp.setLanPreferredBase(defaultPms);
    comp.setPlayerLanHint(comp.localLanIp());
    comp.setLog([](const std::string& s) { misterplex::emitLogSink(s); });
    comp.setPlexTvEnabled(!spelunkrOn && plexTvLinkEnabled);
    comp.setPlexTvToken(plexTvToken);
    comp.setPlexTvLinkPath("/media/fat/misterplex/plex_link.txt");
    comp.setPlexTvPersist([&](const std::string& tok) {
        if (!misterplex::plexui::plexAccountTokenOk(tok))
            return false;
        if (!upsertConfKey(confPath, "PLEXTV_TOKEN", tok)) {
            std::fprintf(stderr, "misterplexd: failed to persist PLEXTV_TOKEN\n");
            return false;
        }
        // Browse v2 is off on a fresh card, and the default conf has no PLEX_BASE.
        // The link is the first moment the account token exists, so discover now.
        applyDiscoveredBase();
        return true;
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
    misterplex::CastTokenBinding castBinding;   // #355: caster token -> /identity-verified host
    bool castBaseUnverified = false;   // #351: cast token must not go to the conf default before verify
    std::string lastToken = readConfToken();
    std::atomic<bool> autoNextInFlight{false};
    // Monotonic play generation: supersede in-flight async resolve when a newer
    // playMedia/auto-next arrives (P4-SCRUB out-of-order bind race).
    std::atomic<uint64_t> playGen{0};
    // Review #208 nit 3: joined (not detached) OSD restore; destroyed before playGen/player.
    misterplex::DelayedTask osdRestore;
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

    // Plex Web can hand the cast target the PMS's IPv6 ULA, which the MiSTer cannot route to. One
    // reachability cache per daemon: every PMS gets one probe round, then the answer is reused.
    misterplex::PmsReachability pmsReach(misterplex::probePmsIdentity);

    auto resolveAgainstServers = [&](const misterplex::PlayRequest& req,
                                     const std::string& preferredBase, int64_t off,
                                     const misterplex::WeakLadder& weakForPlay, int matchW,
                                     int matchH) -> std::pair<misterplex::ResolveResult, std::string> {
        // Cast-pinned host must not authenticate with conf token (different PMS).
        // Conf host with no caster token (or the conf token repeated) uses PLEX_TOKEN.
        std::string token;
        const std::string confTok = readConfToken();
        if (!req.token.empty())
            token = req.token;
        else if (req.address.empty())
            token = confTok;
        else {
            const std::string addrBase =
                std::string(req.protocol.empty() ? "http" : req.protocol) + "://" + req.address + ":" +
                (req.port.empty() ? "32400" : req.port);
            if (misterplex::confTokenOnConfHost(confTok, confTok, addrBase, defaultPms))
                token = confTok;
        }
        // Cast-selected base wins when address present. An IPv6 cast address the MiSTer cannot reach
        // (ULA/link-local, or a global one that does not answer) is replaced by an IPv4/LAN base
        // that answered /identity as the same PMS: plex.tv local connection, configured PMS (conf), and only last
        // the request source IP (the TCP peer: behind a NAT/gateway it is the gateway, not the PMS).
        std::string selected;
        bool fallbackVerified = false;
        // #347: the machine id a base is KNOWN by (linked plex.tv roster); never learned by probing it.
        auto linkedMachineIdFor = [&](const std::string& b) -> std::string {
            const std::string n = misterplex::normalizePlexBase(b);
            for (const misterplex::LinkedServer& s : linkedRoster)
                if (!s.machineIdentifier.empty() && misterplex::normalizePlexBase(s.uri) == n)
                    return s.machineIdentifier;
            return {};
        };
        bool castKept = false;   // #347: cast base kept as is; only the same-host openable map applies
        {
            std::string v6;
            if (!req.address.empty() && misterplex::ipv6FromPmsHost(req.address, v6)) {
                misterplex::PmsFallbackInput fi;
                fi.selected = req.address;
                fi.port = req.port.empty() ? "32400" : req.port;
                fi.machineId = req.serverMachineId;
                fi.peerAddress = req.peerAddress;
                fi.infraHosts = misterplex::pmsInfraHosts("");   // #275: no ladder host
                fi.confBase = !preferredBase.empty() ? preferredBase : defaultPms;
                fi.resourceFn = [&]() {
                    const std::string tvTok = !loadConf(confPath, "PLEXTV_TOKEN").empty()
                                                  ? loadConf(confPath, "PLEXTV_TOKEN")
                                                  : (!readConfToken().empty() ? readConfToken() : token);
                    return misterplex::plexTvLocalIpv4Bases(req.serverMachineId, tvTok, 4);
                };
                std::string why;
                const std::string fb = pmsReach.pick(fi, &why);
                if (!fb.empty() && fb != req.address) {
                    std::fprintf(stderr, "misterplexd: cast IPv6 PMS %s unreachable → %s (%s)\n",
                                 req.address.c_str(), fb.c_str(), why.c_str());
                    selected = fb;
                    fallbackVerified = true;
                } else if (fb.empty()) {
                    std::fprintf(stderr,
                                 "misterplexd: cast IPv6 PMS %s unreachable, no IPv4 fallback answered (%s)\n",
                                 req.address.c_str(), why.c_str());
                }
            }
        }
        if (selected.empty())
            selected = misterplex::buildPlexBase(req.protocol, req.address, req.port, preferredBase);
        if (selected.empty())
            selected = preferredBase.empty() ? defaultPms : preferredBase;
        // Static ARM libav has no HTTPS. Map cast https/*.plex.direct onto an
        // openable HTTP base — but do NOT force every remote shared server onto
        // conf PLEX_BASE (that 404s foreign ratingKeys like home library shows).
        const std::string lanPreferred =
            !preferredBase.empty() ? preferredBase : defaultPms;
        // Rewrite ULA / plex.direct / player-VLAN gateway onto conf PMS
        // (203.0.113.1 next to player  lab → 203.0.113.10). Use defaultPms,
        // not lastBase, as the conf so a polluted gateway lastBase cannot
        // confirm itself.
        const std::string rewriteConf = !defaultPms.empty() ? defaultPms : lanPreferred;
        const std::string playerLan = comp.localLanIp();
        // #333: a cast PMS on the player's /24 is not the gateway when /identity there names the
        // cast machineIdentifier. Keep it; do not rewrite it onto the conf default.
        const bool lanVerified =
            !fallbackVerified &&
            misterplex::castLanPmsVerified(selected, req.serverMachineId, rewriteConf, playerLan,
                                           pmsReach.cachedProbe());
        if (lanVerified) {
            std::fprintf(stderr, "misterplexd: cast base %s kept (player LAN, /identity machine match)\n",
                         misterplex::logUrl(selected).c_str());
            fallbackVerified = true;   // no cross-server rewrite below
            castKept = true;           // same-host https/plex.direct -> http still applies
        } else {
            const std::string prefer =
                misterplex::preferReachablePmsBase({selected}, rewriteConf, playerLan);
            if (!prefer.empty() && prefer != selected &&
                !misterplex::castRewriteVerified(selected, prefer, req.serverMachineId,
                                                 linkedMachineIdFor(prefer), pmsReach.cachedProbe())) {
                std::fprintf(stderr, "misterplexd: cast base %s kept (rewrite target is not a linked candidate "
                             "for this server; no request sent to it)\n",
                             misterplex::logUrl(selected).c_str());
                fallbackVerified = true;   // explicit cast base wins; no cross-server rewrite
                castKept = true;           // same-host https/plex.direct -> http still applies
            } else if (!prefer.empty() && prefer != selected) {
                std::fprintf(stderr, "misterplexd: cast base %s → reachable %s\n",
                             misterplex::logUrl(selected).c_str(), misterplex::logUrl(prefer).c_str());
                selected = prefer;
            }
        }
        const std::string openable =
            castKept           ? misterplex::castKeptOpenableBase(selected)
            : fallbackVerified ? selected
                               : misterplex::companionOpenablePmsBase(selected, rewriteConf, playerLan);
        if (!openable.empty() && openable != selected) {
            const bool keptTls = openable.rfind("https://", 0) == 0;
            std::fprintf(stderr,
                         keptTls ? "misterplexd: cast base %s → openable %s (TLS)\n"
                                 : "misterplexd: cast base %s → openable %s (no companion HTTPS)\n",
                         selected.c_str(), openable.c_str());
            selected = openable;
        } else if (openable.empty()) {
            // Foreign private home LAN (cast address is on another private /24 while conf is lab):
            // look up public HTTP via plex.tv using the cast token + machine id.
            // Never rewrite foreign ratingKeys onto conf PLEX_BASE.
            std::string pub;
            // plex.tv /resources needs the account token. PLEX_TOKEN is the PMS
            // token and is only the fallback. A cast token is often server-scoped.
            const std::string accountTok = loadConf(confPath, "PLEXTV_TOKEN");
            const std::string confNow = readConfToken();
            const std::string lookupTok =
                !accountTok.empty() ? accountTok : (!confNow.empty() ? confNow : token);
            const char* lookupSrc = !accountTok.empty() ? "PLEXTV_TOKEN"
                                    : (!confNow.empty() ? "PLEX_TOKEN" : "cast");
            std::fprintf(stderr,
                         "misterplexd: foreign-private cast lookup machine=%s "
                         "source=%s lookupTok=%zu\n",
                         req.serverMachineId.empty() ? "-" : req.serverMachineId.c_str(),
                         lookupSrc, lookupTok.size());
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
            const std::string castTok =
                misterplex::confTokenOnConfHost(req.token, readConfToken(), base, defaultPms)
                    ? std::string()
                    : req.token;
            if (!castBinding.allow(misterplex::TokenSite::Resolve, base, req.address, castTok, token,
                                   req.serverMachineId, misterplex::probePmsIdentity)) {
                std::fprintf(stderr, "misterplexd: cast token withheld from %s (not the cast server; "
                             "/identity does not name it)\n", base.c_str());
                misterplex::ResolveResult no;
                no.detail = "cast token withheld: unverified server";
                return no;
            }
            misterplex::pendingPlayMediaSubtitleId() = req.hasSubtitleStreamId ? req.subtitleStreamId : -1;
            misterplex::pendingPlayMediaAudioId() = req.hasAudioStreamId ? req.audioStreamId : -1;
            auto r = misterplex::resolvePlayTarget(req.key, base, token, off, /*weakAlways=*/true,
                                                   weakForPlay,
                                                   /*preferDirectH264=*/streamEnabled, matchW,
                                                   matchH, req.forceLadder, req.gateDetail,
                                                   markerFlags.markers && !req.ratingKey.empty());
            misterplex::pendingPlayMediaSubtitleId() = -1;
            misterplex::pendingPlayMediaAudioId() = -1;
            return r;
        };

        // 720p24: Plex Web pins address=LAN PMS. That 401s the conf token and
        // skips the host 9324 proxy (x86 1280x720 baseline @ 24p). Cast then
        // played the wrong source or PMS HEVC→720 (~13 unique). Prefer proxy.
        if (player.liveGlass() == misterplex::LiveGlass::L4 &&
            preferredBase.find("127.0.0.1:9324") != std::string::npos &&
            castBinding.allow(misterplex::TokenSite::Proxy, preferredBase, req.address, req.token,
                                          token.empty() ? readConfToken() : token, req.serverMachineId,
                                          misterplex::probePmsIdentity)) {   // #351: else skip the proxy
            std::string t = token;
            if (t.empty())
                t = readConfToken();
            misterplex::pendingPlayMediaSubtitleId() = req.hasSubtitleStreamId ? req.subtitleStreamId : -1;
            misterplex::pendingPlayMediaAudioId() = req.hasAudioStreamId ? req.audioStreamId : -1;
            auto proxied = misterplex::resolvePlayTarget(
                req.key, preferredBase, t, off, /*weakAlways=*/true, weakForPlay,
                /*preferDirectH264=*/streamEnabled, matchW, matchH, req.forceLadder, req.gateDetail,
                markerFlags.markers && !req.ratingKey.empty());
            misterplex::pendingPlayMediaSubtitleId() = -1;
            misterplex::pendingPlayMediaAudioId() = -1;
            if (proxied.ok) {
                std::fprintf(stderr,
                             "misterplexd: 720p24 PLEX_BASE proxy %s (cast address=%s)\n",
                             misterplex::logUrl(preferredBase).c_str(),
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

    // Review 156a264 nit 3: remote direct play with no decoded picture -> ONE 480p transcode replay.
    std::function<void(const misterplex::PlayRequest&)> remoteDirectReplay;
    misterplex::DelayedTask remoteDirectWatch;
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
        // R1 (FPGA Bug 06:57 D3, 07:30 C1, 08:17 (2)): no PLXH en=1 from this PLAY request to the
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
        // Chevron from this moment through resolve. A failure return (nothing queued)
        // restores IDLE. A started play keeps the cue until the first HDMI picture.
        struct PlayPendingExit {
            misterplex::MediaPlayer* player = nullptr;
            uint64_t gen = 0;
            bool keep = false;
            ~PlayPendingExit() {
                if (!keep && player && gen != 0)
                    player->abandonPlayPending(gen);
            }
        } playPendingExit{&player, gen, false};
        player.armPlayPendingCue(gen, misterplex::gcast_osd::pendingLoadingLine("Plex"));
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
        // MPX_BROWSE_V2: a browse ticket's ladder input (reviewer Q4: the helper's URL, or the
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
            // Resolve failure closes the play. There is no test-pattern or other fallback source.
            std::lock_guard<std::mutex> failedHandoff(playHandoffMu);
            if (gen != playGen.load() || !comp.acceptsPlayRequest(req))
                return;
            misterplex::retirePlayback(player, pmsTimeline);
            if (gen != playGen.load())
                return;
            std::fprintf(stderr, "misterplexd: resolve failed: %s\n", resolved.detail.c_str());
            closedStopPlay("resolve failed; no fallback", off, resolved.durationMs);
            // #226: forced remote transcode abandoned — error OSD before READY, not a silent idle card.
            if (misterplex::remoteTranscodeFailed(resolved.detail)) {
                const std::string osd =
                    misterplex::remoteTranscodeFailOsdDetail(resolved.remoteUniversalStartHttp);
                std::fprintf(stderr,
                             "misterplexd: remote transcode failure -> READY with error OSD http=%d: %s\n",
                             resolved.remoteUniversalStartHttp, osd.c_str());
                player.showRemoteTranscodeFailure(gen, osd);
                playPendingExit.keep = true;   // showRemoteTranscodeFailure already cleared PLAY-PENDING
            }
            return;
        } else {
            std::fprintf(stderr, "misterplexd: resolved %s title=%s dur=%lld transcode=%d base=%s\n",
                         resolved.detail.c_str(), resolved.title.c_str(),
                         static_cast<long long>(resolved.durationMs),
                         resolved.transcoded ? 1 : 0, misterplex::logUrl(base).c_str());
        }
        if (misterplex::remoteDirectArmWatch(resolved.transcoded, resolved.detail, req.seekReplay)) {
            const std::string rdKey = misterplex::remoteDirectLastKey();
            remoteDirectWatch.schedule(
                std::chrono::milliseconds(misterplex::kRemoteDirectFirstPicMs),
                [&player, &playGen, &playHandoffMu, &remoteDirectReplay, gen, req, rdKey] {
                    const bool pic = player.owenbDecodedPicture();
                    if (!misterplex::remoteDirectPicFallback(true, pic, misterplex::remoteDirectForced(rdKey),
                                                             gen == playGen.load()))
                        return;
                    uint64_t next = 0;
                    {   // CAS gen -> gen+1 with its R1 hold: a PLAY/STOP/seek that landed first wins.
                        std::lock_guard<std::mutex> handoff(playHandoffMu);
                        next = player.r1BeginPlayRequest(
                            [&]() -> uint64_t { return misterplex::remoteDirectClaimGen(playGen, gen); });
                        if (next != 0)
                            player.armPlayPendingCue(next, misterplex::gcast_osd::pendingLoadingLine("Plex"));
                    }
                    if (next == 0) {
                        std::fprintf(stderr, "misterplexd: remote direct fallback: superseded (gen %llu), no replay\n",
                                     static_cast<unsigned long long>(gen));
                        return;
                    }
                    std::fprintf(stderr,
                                 "misterplexd: remote direct fallback: no decoded picture in %d ms key=%s; "
                                 "replaying ONCE as the 480p transcode\n",
                                 misterplex::kRemoteDirectFirstPicMs, rdKey.c_str());
                    misterplex::setRemoteDirectForcedKey(rdKey);
                    misterplex::PlayRequest again = req;
                    again.dispatchGeneration = next;
                    again.seekReplay = false;
                    // doPlay stops/joins player threads; never on this timer thread (as the PLAY dispatch).
                    std::thread([&remoteDirectReplay, again] { remoteDirectReplay(again); }).detach();
                });
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

        // Advice only. Source facts are the PMS rational, before the 30 Hz pacing cap.
        // pictureAdviceLog does not load a core and does not write video_mode.
        // The phone page still refreshes from summaryLine when the resolved mode is advise.
        {
            misterplex::autopic::Source src;
            src.width = resolved.mediaWidth;
            src.height = resolved.mediaHeight;
            src.interlaced = resolved.mediaInterlaced;
            src.fpsNum = resolved.fpsNum;
            src.fpsDen = resolved.fpsDen;
            const std::string paired = misterplex::CoreGate::process().pairedRaster();
            const misterplex::autopic::PictureLog advised =
                misterplex::autopic::pictureAdviceLog(src, paired, autoPictureMode);
            if (advised.emit) {
                std::fprintf(stderr, "misterplexd: %s\n", advised.line.c_str());
                const auto dec = misterplex::autopic::classify(src);
                const auto adv = misterplex::autopic::advise(dec, misterplex::autopic::rasterFromName(paired));
                lastAutoPic.set(misterplex::autopic::summaryLine(src, dec, adv));
            }
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
                queueToken = readConfToken();
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
        comp.notePhoneTitle(resolved.title);
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
        // Timeline.address is the PMS (so Web can fetch metadata). Always publish
        // the reachable LAN IPv4 the player opened — not the playMedia ULA /
        // plex.direct host, and not this box's companion IP.
        if (!base.empty()) {
            std::string addr, pmsPort, proto;
            if (misterplex::applyPmsBaseToEndpoint(base, addr, pmsPort, proto)) {
                if (bound.address != addr || bound.port != pmsPort || bound.protocol != proto) {
                    std::fprintf(stderr,
                                 "misterplexd: companion timeline PMS %s://%s:%s → %s://%s:%s\n",
                                 bound.protocol.empty() ? "-" : bound.protocol.c_str(),
                                 bound.address.empty() ? "-" : bound.address.c_str(),
                                 bound.port.empty() ? "-" : bound.port.c_str(), proto.c_str(),
                                 addr.c_str(), pmsPort.c_str());
                } else {
                    std::fprintf(stderr, "misterplexd: companion timeline PMS %s://%s:%s\n",
                                 proto.c_str(), addr.c_str(), pmsPort.c_str());
                }
                bound.address = addr;
                bound.port = pmsPort;
                bound.protocol = proto;
                comp.setTimelinePmsBase(base);
            }
        }
        comp.setPlayDecision(resolved.transcoded ? "transcode" : "direct");
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
            castBaseUnverified = false;
            seekReuse = misterplex::makeSeekReuseEntry(bound, weakForPlay, 0, resolved, base);
            // SPS-gate L3: the exact request this generation committed (the gate fallback re-plays only this one).
            committedPlay = bound;
            committedPlayGen = gen;
            if (!bound.token.empty())
                lastToken = bound.token;
            else if (req.address.empty()) {
                const std::string confNow = readConfToken();
                if (!confNow.empty())
                    lastToken = confNow;
            }
        }

        // Timeline must target the server that issued playMedia, with THAT request's
        // credentials. Mixing cast host (e.g. *.plex.direct) with conf PLEX_TOKEN from
        // a different PMS yields HTTP 401 while the old !body.empty() sink still looked ok
        // (docs/evidence/timeline-stuck-20260730T173000Z / eeae7943).
        std::string timelineToken;
        const char* tokenSrc = "none";
        const std::string confNow = readConfToken();
        if (!bound.token.empty()) {
            timelineToken = bound.token;
            tokenSrc = misterplex::confTokenOnConfHost(bound.token, confNow, base, defaultPms) ? "conf" : "cast";
        } else if (req.address.empty() ||
                   misterplex::confTokenOnConfHost(confNow, confNow, base, defaultPms)) {
            if (!confNow.empty()) {
                timelineToken = confNow;
                tokenSrc = "conf";
            }
        }
        misterplex::PmsTimelineSession timelineSession;
        // Plex Web via http://127.0.0.1:32400 plants address=127.0.0.1 on
        // playMedia. That is the PMS host's loopback, not this MiSTer. Keep the
        // advertised timeline XML address as-cast but send GET /:/timeline to a
        // host the player can reach (conf PLEX_BASE / last base).
        const std::string lanForTimeline = !defaultPms.empty() ? defaultPms : lastBase;
        timelineSession.baseUrl = misterplex::rewriteLoopbackPmsBase(base, lanForTimeline);
        if (timelineSession.baseUrl != base)
            std::fprintf(stderr, "misterplexd: pms timeline loopback %s -> %s\n",
                         misterplex::logUrl(base).c_str(), misterplex::logUrl(timelineSession.baseUrl).c_str());
        // #35: IPv6-unreachable base falls back to the IPv4 PMS address. #347: only for a play with no
        // cast address; a cast from another server never sends its timeline to the conf default.
        // #351: the caster token goes to the timeline base only after the same gate; never to a fallback.
        const bool castTl = !req.token.empty() && timelineToken == req.token && std::strcmp(tokenSrc, "cast") == 0;
        if (castTl &&
            !misterplex::confTokenOnConfHost(req.token, confNow, timelineSession.baseUrl, defaultPms) &&
            !castBinding.allow(misterplex::TokenSite::Timeline, timelineSession.baseUrl, req.address, req.token,
                               timelineToken, req.serverMachineId, misterplex::probePmsIdentity)) {
            std::fprintf(stderr, "misterplexd: pms timeline cast token withheld (unverified server)\n");
            timelineToken.clear();
        }
        timelineSession.fallbackBase = (req.address.empty() && !castTl) ? defaultPms : std::string();
        timelineSession.token = timelineToken;
        timelineSession.key = bound.key;
        timelineSession.ratingKey = bound.ratingKey;
        timelineSession.playQueueItemId = bound.playQueueItemId;
        timelineSession.containerKey = bound.containerKey;
        timelineSession.serverMachineId = bound.serverMachineId;
        timelineSession.clientIdentifier = machineId;
        timelineSession.sessionIdentifier =
            misterplex::pmsTimelineSessionIdentifier(resolved.sessionId, machineId, gen);
        // The transcode session the PMS owns for this play (empty for a direct play): the reporter pings
        // it every 10 s and stops it after the final stopped timeline, to the SAME base and token as the
        // timeline. fr-heartbeat-1025: the daemon never pinged or stopped it before.
        // fr-resume-1025: only when the play IS the transcode. A direct-Part fallback after a dead remote start
        // carries the abandoned session's id; pinging or stopping it as this play's session is wrong (the
        // resolve already stopped it).
        timelineSession.transcodeSessionId = misterplex::remote_start::heartbeatSession(resolved.transcoded, resolved.sessionId);
        timelineSession.product = "MiSTerPlex";
        timelineSession.clientProfileName = !resolved.clientProfileName.empty()
                                                ? resolved.clientProfileName
                                                : misterplex::defaultRequestProfileName();
        timelineSession.version = "0.4.1";
        timelineSession.deviceName = name;
        std::fprintf(stderr,
                     "misterplexd: pms timeline session base=%s token_src=%s ratingKey=%s\n",
                     misterplex::logUrl(timelineSession.baseUrl).c_str(), tokenSrc, timelineSession.ratingKey.c_str());
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
        player.armTrackSelection(resolved.trackPick, resolved.tracks, resolved.showKey, resolved.partId, base,
                                 timelineToken, resolved.transcodeUrl, resolved.softSubPath, resolved.partKey);
        if (markerFlags.markers && !resolved.ratingKey.empty()) {
            misterplex::MediaPlayer::MarkerArm arm;
            arm.markers = resolved.markers;
            arm.durationMs = resolved.durationMs;
            arm.cfg = misterplex::markers::toConfig(markerFlags, autoNext);
            arm.preserveConsumed = req.forceLadder || req.fallbackUsed || req.pmsLowered;
            arm.userLanded = req.seekReplay;
            arm.ratingKey = resolved.ratingKey;
            arm.transcoded = resolved.transcoded;
            arm.queued = !req.playQueueId.empty() || req.containerKey.find("/playQueues/") != std::string::npos;
            const auto cur = req;
            const auto meta = resolved;
            const std::string nextBase = base;
            const std::string nextToken = timelineToken;
            arm.resolveNext = [cur, meta, nextBase, nextToken] {
                return misterplex::resolveNextItem(cur, meta, nextBase, nextToken);
            };
            player.armMarkers(std::move(arm));
        } else {
            player.clearMarkers();
        }
        if (misterplex::chaptersEnabled())
            player.armChapters(resolved.chapters);
        else
            player.clearChapters();
        if (misterplex::scrubThumbsEnabled() && resolved.partId > 0 && !resolved.scrubIndexKind.empty())
            player.armScrubIndex(base, timelineToken, resolved.partId, resolved.scrubIndexKind);
        else
            player.clearScrubIndex();
        std::fprintf(stderr,
                     "misterplexd: subtitle pick id=%d off=%d codec=%s\n",
                     resolved.trackPick.subtitleStreamId, resolved.trackPick.subtitlesOff ? 1 : 0,
                     resolved.trackPick.subtitleCodec.empty() ? "-" : resolved.trackPick.subtitleCodec.c_str());
        std::fprintf(stderr, "misterplexd: PLAY %s off=%lld dur=%lld\n",
                     misterplex::logUrl(resolved.playable).c_str(),
                     static_cast<long long>(startAt), static_cast<long long>(resolved.durationMs));
        if (!player.play(resolved.playable, startAt, resolved.httpHeaders,
                         resolved.durationMs, gen, gateCtx)) {
            std::fprintf(stderr, "misterplexd: PLAY failed to start demux key=%s\n",
                         bound.key.c_str());
            closedStopPlay("PLAY failed to start demux", startAt, resolved.durationMs);
            return;
        }
        playPendingExit.keep = true;   // picture (or stop/fail) owns the cue from here
        r1PlayHold.started();   // the session of gen takes the hold over at its enter
        fpgaWorkers.completePlayback();
    };

    // GCAST (P1/P2): AirPlay / YouTube cast plays through the SessionArbiter. With GAIRPLAY and
    // GYT off (the default) nothing below runs and the Plex path is unchanged.
    misterplex::gcast::SessionArbiter gcastArbiter;
    const bool gcastAirplay = !spelunkrOn && misterplex::envFlagWanted("GAIRPLAY");
    const bool gcastYt = !spelunkrOn && misterplex::envFlagWanted("GYT");
    const bool gcastOn = gcastAirplay || gcastYt;
    bool gcastApUp = false;
    std::atomic<bool> gcastYtUp{false};   // P4: YouTube ready flips from the Lounge thread and the main loop
    std::atomic<bool> gcastApReady{false};   // Ready-card AirPlay: helper up and /probe self-test passed
    std::atomic<uint64_t> castGen{0};
    std::atomic<uint64_t> castToken{0};
    std::atomic<uint64_t> apHandoffHeldGen{0};   // gen of a cast holding the Plex picture (paused) until its handoff
    std::atomic<bool> apMirrorHeldPlex{false};   // a mirror start paused a Plex play; the first AU stops it
    std::atomic<bool> ytLiveSession{false};     // lounge duration floor while a live remux is the play
    auto castPlay = [&](misterplex::gcast::Owner owner, const std::string& url, double startS,
                        const std::string& title, int64_t durHintMs, int64_t resumeMs,
                        bool isSeek = false) -> bool {
        using namespace misterplex::gcast;
        struct CastPendingExit {
            misterplex::MediaPlayer* player = nullptr;
            uint64_t gen = 0;
            bool armed = false;
            bool keep = false;
            ~CastPendingExit() {
                if (armed && !keep && player && gen != 0)
                    player->abandonPlayPending(gen);
            }
        } castPending{&player};
        uint64_t gen;
        {
            std::lock_guard<std::mutex> handoff(playHandoffMu);
            gen = player.r1BeginPlayRequest([&] { return ++playGen; });   // in-flight Plex doPlay aborts
        }
        castGen.store(gen);
        castPending.gen = gen;
        // AirPlay/YT accept, before resolve. RAOP keeps the idle card (keepIdleUntilPicture).
        // A seek is a re-play of a picture already on glass.
        if (!isSeek && !raopCastUrl(url)) {
            player.armPlayPendingCue(gen, misterplex::gcast_osd::pendingLoadingLine(ownerName(owner)));
            castPending.armed = true;
        }
        const uint64_t tok = gcastArbiter.claim(owner);   // retires Plex (closedStop) or the other caster
        castToken.store(tok);
        // Every exit before the real handoff stops a held Plex play (idle card), as the early stop did.
        HandoffHold handoffHold([&, gen] {
            std::lock_guard<std::mutex> handoff(playHandoffMu);
            uint64_t heldGen = gen;
            apHandoffHeldGen.compare_exchange_strong(heldGen, 0);
            if (gen == playGen.load())
                misterplex::retirePlayback(player, pmsTimeline);
        });
        // Gate 2b: a skip is a new play while the previous stream is still on glass.
        // Stop it before yt-dlp. playGen already moved and activePlayGen still names
        // the old play, so this stop's progress is not applied to the new Lounge id.
        // A failed resolve then leaves the chevron, matching the Stopped state the sink reports.
        {
            const auto tear = castGlassAction(GlassStep::NewPlay, player.playing(), true, owner,
                                              gcastArbiter.owner(), true, !isSeek);
            if (tear.stopMedia && holdPlexForHandoff(owner, player.playing(), isSeek)) {
                // AirPlay start keeps the last Plex frame: pause (picture held), stop at the real handoff.
                std::lock_guard<std::mutex> handoff(playHandoffMu);
                if (gen == playGen.load()) {
                    player.pause();
                    apHandoffHeldGen.store(gen);
                    handoffHold.arm();
                }
            } else if (tear.stopMedia) {
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
            const std::string body = misterplex::plexHttpGet(remote + "/health", 2, misterplex::gcast::helperCurlHeaders());
            const bool ok = body.find("ok") != std::string::npos;
            xcodeHealthCache = ok ? remote : ("!" + remote);
            xcodeHealthAt = now;
            if (!ok)
                std::fprintf(stderr, "misterplexd: AIRPLAY_XCODE down; trying standalone\n");
            return ok;
        };
        const bool remoteOk = xcodeHealthy();
        const bool localOk = remoteOk ? false : misterplex::gcast::cachedLoopbackHelperHealth();
        const std::string xcode = effectiveXcodeBase(remoteOk, localOk);
        // #275: no ladder. FPGA non-YouTube keeps an empty helper. Direct http
        // is not played on the FPGA. xcode stays the non-FPGA path.
        const std::string helper = fpga ? std::string() : xcode;
        std::string ytId;
        const bool ytSrc = ytCastId(url, ytId);
        if (ytSrc) {
            std::string clockErr;
            misterplex::WallClockSource clockSrc = misterplex::WallClockSource::None;
            const auto clock = misterplex::waitForWallClockRescue(misterplex::kWallClockRescueWaitMs, &clockErr, &clockSrc);
            if (clock == misterplex::WallClockStatus::Unavailable) {
                // Not the shared 5-minute clock limiter: a refusal during gate 8
                // must still be visible after a rescue line.
                std::fprintf(stderr, "misterplexd: YouTube refused: %s\n", clockErr.c_str());
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
            if (route.xcode.empty() && misterplex::gcast::helperOriginIsLoopback(xcode))
                std::fprintf(stderr, "misterplexd: YouTube uses yt-dlp (on-card helper is AirPlay only)\n");
            if (xcode.empty() && !route.localYtdlp)
                std::fprintf(stderr, "misterplexd: yt-dlp not executable (looked in %s)\n",
                             yt_dlp::searchedPaths().c_str());
        }
        CastPlan plan = planGcastPlay(route);
        yt_dlp::ResolveOptions yopt;
        bool ytLiveRemux = false;
        std::string ytExtractTitle;
        if (ytSrc && plan.playable.compare(0, 9, "yt-local:") == 0) {
            const yt_dlp::FoundBinary bin = yt_dlp::findBinary(pyMaj, pyMin, yt_dlp::candidatePaths(), ytdlpPrefP);
            yopt.bin = bin.path;
            yopt.zipapp = bin.kind == yt_dlp::ReleaseKind::Zipapp;
            yopt.videoId = ytId;
            // YT_LIVE appends HLS fallbacks after the https selectors. Unset
            // leaves format empty, so the VOD -g argv is unchanged.
            if (yt_live::ytLiveEnabled())
                yopt.format = yt_live::liveEnabledFormat();
            // A seek is a re-play of the same video: reuse the resolved URLs until shortly before they
            // expire instead of a 19-22 s yt-dlp run (resolveVideoCached; 403 / open failure re-resolves).
            bool ytCacheHit = false;
            const yt_dlp::ResolveResult yr =
                yt_dlp::resolveVideoCached(yopt, yt_dlp::resolveCache(), std::time(nullptr), false, &ytCacheHit);
            std::fprintf(stderr, "%s\n", yt_dlp::resolveLogLine(yr, ytCacheHit).c_str());   // no resolved URL
            auto ytFail = [&]() {
                if (yr.extractionError)
                    yt_dlp::scheduleDailyUpdate(yt_dlp::configFromEnv());
                const auto fail = castGlassAction(GlassStep::ResolveFailed, player.playing(), !ytId.empty(),
                                                  owner, gcastArbiter.owner());
                if (fail.stopMedia) {
                    std::lock_guard<std::mutex> handoff(playHandoffMu);
                    if (gen == playGen.load())
                        misterplex::retirePlayback(player, pmsTimeline);
                    ytLiveSession.store(false);
                    yt_live::liveRelay().stop();
                }
                gcastArbiter.release(owner, tok);
                if (gen == playGen.load()) {
                    const std::string ready = misterplex::gcast_osd::gcastIdleHint(
                        gcastApReady.load(), gcastYtUp.load(),
                        misterplex::gcast_osd::gcastLinkCodeForIdle(
                            misterplex::plexui::plexAccountTokenOk(plexTvToken), comp.plexTvCode()));
                    player.setIdleReadyLine(yt_dlp::resolveFailureIdleLine(yr));
                    std::thread([&player, ready, gen, &playGen] {
                        std::this_thread::sleep_for(std::chrono::seconds(10));
                        if (gen == playGen.load())
                            player.setIdleReadyLine(ready);
                    }).detach();
                }
                return false;
            };
            if (!yr.ok)
                return ytFail();
            ytExtractTitle = yr.title;
            if (yt_live::ytLiveEnabled() && yt_live::hlsManifestUrl(yr.url)) {
                // Lounge currentTime is broadcast time. Play the live edge.
                yt_live::LiveSource src;
                src.ffmpeg = yt_live::findLiveFfmpeg();
                src.videoUrl = yr.url;
                src.audioUrl = yr.audioUrl;
                std::string liveErr;
                const std::string relay = yt_live::liveRelay().start(src, liveErr);
                if (relay.empty()) {
                    std::fprintf(stderr, "misterplexd: yt live remux failed: %s\n", liveErr.c_str());
                    return ytFail();
                }
                plan.playable = yt_live::livePlayable(true, yr.url, relay);
                plan.why = "yt live HLS remux (MPEG-TS)";
                ytLiveRemux = true;
                ytLiveSession.store(true);
                std::fprintf(stderr, "misterplexd: yt live remux HLS to MPEG-TS\n");
            } else {
                ytLiveSession.store(false);
                yt_live::liveRelay().stop();
                plan.playable = yr.audioUrl.empty() ? yr.url : ytAvPlayable(yr.url, yr.audioUrl);
            }
        }
        const int64_t playAtMs = yt_live::livePlayStartMs(ytLiveRemux, startMs);
        if (plan.route == Route::Helper && fpga)
            plan.playable += "&fps=src";
        std::fprintf(stderr, "misterplexd: GCAST %s play gen=%llu backend=%s raster=%dx%d br=%d start=%lld route=%d (%s)\n",
                     ownerName(owner), static_cast<unsigned long long>(gen), fpga ? "fpga" : "owenb", lw, lh, br,
                     static_cast<long long>(playAtMs), static_cast<int>(plan.route), plan.why.c_str());
        if (plan.route == Route::Refuse) {
            gcastArbiter.release(owner, tok);
            return false;
        }
        // FIELD-1018 F4: FairPlay / Apple TV+ never plays; say so on screen now instead of a 30 s probe + open timeout.
        auto castRefuse = [&](const char* msg) {
            std::fprintf(stderr, "misterplexd: GCAST %s refused gen=%llu: %s\n", ownerName(owner),
                         static_cast<unsigned long long>(gen), msg);
            gcastArbiter.release(owner, tok);
            const std::string ready = misterplex::gcast_osd::gcastIdleHint(
                gcastApReady.load(), gcastYtUp.load(),
                misterplex::gcast_osd::gcastLinkCodeForIdle(
                    misterplex::plexui::plexAccountTokenOk(plexTvToken), comp.plexTvCode()));
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
            pi = parseProbe(misterplex::plexHttpGet(probeAt + "/probe?src=" + encodeComponent(url), 90,
                                                     misterplex::gcast::helperCurlHeaders()));
        if (owner == Owner::AirPlay && !xcode.empty() &&
            castSourceVerdict(url, true, pi.ok) == CastSourceVerdict::Unplayable)
            return castRefuse(kUnplayableMsg);
        const int64_t durMs = pi.durMs > 0 ? pi.durMs : durHintMs;
        std::fprintf(stderr, "misterplexd: GCAST probe ok=%d src=%dx%d vcodec=%s audio=%d dur=%lld\n", pi.ok ? 1 : 0,
                     pi.w, pi.h, pi.vcodec.c_str(), pi.audio ? 1 : 0, static_cast<long long>(durMs));
        std::unique_lock<std::mutex> handoff(playHandoffMu);
        if (gen != playGen.load()) {
            std::fprintf(stderr, "misterplexd: GCAST play superseded gen=%llu\n", static_cast<unsigned long long>(gen));
            if (ytLiveRemux) {
                ytLiveSession.store(false);
                yt_live::liveRelay().stop();
            }
            return false;
        }
        handoffHold.commit();   // the real handoff: the held Plex play stops here, then the AirPlay play opens
        {
            uint64_t heldGen = gen;
            apHandoffHeldGen.compare_exchange_strong(heldGen, 0);
        }
        misterplex::retirePlayback(player, pmsTimeline);
        FpgaWorkerHandoff fpgaWorkers(player);
        if (fpga)
            player.setDecodeSize(contentRes.width, contentRes.height);
        else
            player.setDecodeSize(bankRes.width, bankRes.height);
        player.setContentFpsRational(0, 0);
        // P4: now-playing names the owner ("YouTube: <title>"); the OSD shows it on pause/seek.
        // YT_CAST_TITLE off keeps probe title, else the passed fallback ("YouTube" for a cast).
        // On, a local extract title replaces that for YouTube only. AirPlay is unchanged.
        {
            const std::string titleForOsd = yt_dlp::ytCastShownTitle(
                owner == Owner::YouTube, yt_dlp::ytCastTitleEnabled(), ytExtractTitle,
                !pi.title.empty() ? pi.title : title);
            const std::string shownTitle = misterplex::gcast_osd::gcastNowPlaying(ownerName(owner), titleForOsd);
            player.setPlaybackTitle(shownTitle);
            comp.notePhoneTitle(shownTitle);
        }
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
                if (ytLiveRemux) {
                    ytLiveSession.store(false);
                    yt_live::liveRelay().stop();
                }
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
        const int64_t playDurMs = ytLiveRemux ? static_cast<int64_t>(0) : durMs;
        std::fprintf(stderr, "misterplexd: GCAST PLAY %s off=%lld dur=%lld\n",
                     misterplex::redactUrlTokens(plan.playable).c_str(), static_cast<long long>(playAtMs),
                     static_cast<long long>(playDurMs));
        if (owner == Owner::YouTube) {
            player.armCastOpenRetry(resumeMs);
            std::string yid = ytId;
            yt_dlp::ResolveOptions yAgain = yopt;
            player.armYt403Reresolve([yid, yAgain](std::string& playUrl, std::string& audio) mutable {
                yAgain.videoId = yid;
                // P2-c: android/warm URLs can 403 without a GVS PO token; skip warm + android
                // and let the ladder try default/mweb/tv on refresh.
                yAgain.skipWarm = true;
                yAgain.skipAndroid = true;
                // 403: the cached URLs are stale, so skip the cache and store the fresh pair.
                const yt_dlp::ResolveResult again =
                    yt_dlp::resolveVideoCached(yAgain, yt_dlp::resolveCache(), std::time(nullptr), true);
                if (!again.ok)
                    return false;
                if (yt_live::ytLiveEnabled() && yt_live::hlsManifestUrl(again.url)) {
                    yt_live::LiveSource src;
                    src.ffmpeg = yt_live::findLiveFfmpeg();
                    src.videoUrl = again.url;
                    src.audioUrl = again.audioUrl;
                    std::string err;
                    const std::string relay = yt_live::liveRelay().start(src, err);
                    if (relay.empty())
                        return false;
                    playUrl = yt_live::livePlayable(true, again.url, relay);
                    audio.clear();
                    return true;
                }
                yt_live::liveRelay().stop();
                playUrl = again.url;
                audio = again.audioUrl;
                return true;
            });
        } else {
            player.armYt403Reresolve(nullptr);
        }
        if (misterplex::chaptersEnabled())
            player.clearChapters();
        player.clearScrubIndex();
        if (!player.play(plan.playable, playAtMs, misterplex::gcast::helperPlayHeaders(plan.playable), playDurMs, gen, gateCtx)) {
            std::fprintf(stderr, "misterplexd: GCAST PLAY failed to start demux err=%s\n",
                         misterplex::logUrl(player.lastError()).c_str());
            if (ytLiveRemux) {
                ytLiveSession.store(false);
                yt_live::liveRelay().stop();
            }
            if (owner == Owner::YouTube && !ytId.empty())
                yt_dlp::resolveCache().invalidate(ytId);   // do not reuse URLs that did not open
            gcastArbiter.release(owner, tok);
            return false;
        }
        r1PlayHold.started();
        fpgaWorkers.completePlayback();
        castPending.keep = true;
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
        ytLiveSession.store(false);
        misterplex::gcast::yt_live::liveRelay().stop();
        std::lock_guard<std::mutex> handoff(playHandoffMu);
        player.abandonPlayPending(castGen.load());   // cast stop: IDLE card, no big chevron
        misterplex::retirePlayback(player, pmsTimeline);
        if (cur == owner)
            gcastArbiter.release(owner, castToken.load());
        std::fprintf(stderr, "misterplexd: GCAST %s stop\n", misterplex::gcast::ownerName(owner));
    };

    // Live content-res change (OSD O[5:4]): sync conf, retarget PMS weak ladder,
    // restart session at same playhead. Display (O[15:14]) writes video_mode in
    // applyOsd and only upserts DISPLAY_RES here — no DECODE / play restart.
    // The picker (the active browse target) chooses which PMS. Linked: that
    // server's resources accessToken. Not linked: the target token, else
    // PLEX_TOKEN. The plex.tv account token is never sent to the PMS. The
    // credential is a header, not a query, so a log of the URL cannot carry it.
    player.setPlexBrowseTargets(browseTargets);
    auto libraryCredential = [&]() -> std::string {
        const auto active = player.activePlexBrowseTarget();
        const std::string account = loadConf(confPath, "PLEXTV_TOKEN");
        const bool linked = misterplex::plexui::plexAccountTokenOk(account);
        if (linked && linkedRoster.empty())
            applyDiscoveredBase();
        const std::string base = !active.baseUrl.empty() ? active.baseUrl : defaultPms;
        if (linked)
            return misterplex::credentialForLinkedTarget(linkedRoster, active.machineId, base,
                                                        active.token, readConfToken());
        if (!active.token.empty())
            return active.token;
        return misterplex::libraryToken(false, {}, readConfToken());
    };
    if (!spelunkrOn) {
    player.setLibraryFetch([&](const std::string& path) {
        const auto active = player.activePlexBrowseTarget();
        if (defaultPms.empty() && active.baseUrl.empty())
            applyDiscoveredBase();
        const std::string base = !active.baseUrl.empty() ? active.baseUrl : defaultPms;
        const misterplex::LibraryFetchPlan planned =
            misterplex::planLibraryFetch(base, path, libraryCredential());
        std::vector<std::pair<std::string, std::string>> hdrs;
        if (!planned.headerToken.empty())
            hdrs.push_back({"X-Plex-Token", planned.headerToken});
        return misterplex::plexHttpGet(planned.url, 15, hdrs);
    });
    player.setLibraryLink(
        [&] {
            return !readConfToken().empty() || !loadConf(confPath, "PLEXTV_TOKEN").empty();
        },
        [&] { return comp.plexTvCode(); });
    player.setLibraryPlay([&](const std::string& rk) {
        if (defaultPms.empty())
            applyDiscoveredBase();
        misterplex::PlayRequest req;
        req.key = misterplex::librarylist::browseListPlayMediaKey(rk);
        req.ratingKey = misterplex::librarylist::browseListPlayRatingKey(rk);
        req.token = libraryCredential();
        const auto active = player.activePlexBrowseTarget();
        if (player.plexBrowseTargetCount() > 1 && !active.baseUrl.empty()) {
            std::string proto, host, port;
            if (misterplex::splitPlexBase(active.baseUrl, proto, host, port)) {
                req.protocol = proto;
                req.address = host;
                req.port = port;
            }
            req.serverMachineId = active.machineId;
        }
        const bool linked = misterplex::plexui::plexAccountTokenOk(loadConf(confPath, "PLEXTV_TOKEN"));
        std::fprintf(stderr, "misterplexd: library overlay play ratingKey=%s source=%s server=%s\n",
                     rk.c_str(), linked ? "linked" : "conf",
                     active.name.empty() ? "-" : active.name.c_str());
        {
            std::lock_guard<std::mutex> lock(sessionMu);
            lastPlay = req;
            if (!req.token.empty())
                lastToken = req.token;
        }
        comp.stagePlay(req);
        {
            std::lock_guard<std::mutex> handoff(playHandoffMu);
            const uint64_t gen = player.r1BeginPlayRequest([&] { return ++playGen; });   // R1 C1: held from here
            player.armPlayPendingCue(gen, misterplex::gcast_osd::pendingLoadingLine("Plex"));
        }
        // doPlay -> stop() / FpgaWorkerHandoff joins OSD+input; never run on those threads.
        std::thread([&, req]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            doPlay(req);
        }).detach();
    });
    }
    // MPX_BROWSE_V2 browse play (browse_controller.hpp): the request a phone cast makes. The
    // ticket's server (empty: the conf PMS and token) and its start: offsetPresent makes
    // doPlay skip the PMS viewOffset, because the RESUME / PLAY FROM START row chose already.
    // Remote rows resolve on their own server like a cast, so the ladder, the source rate
    // and the owner's timeline all come from doPlay unchanged. Never logs the token.
    if (!spelunkrOn && misterplex::plexui::browsePlayWanted(plexUiOn, browseV2Setting)) {
        std::lock_guard<std::mutex> lk(browsePlayMu);
        browsePlay = [&](const misterplex::BrowsePlayTicket& t) -> uint64_t {
            misterplex::PlayRequest req;
            req.ratingKey = t.ratingKey;
            req.key = t.key.empty() ? "/library/metadata/" + t.ratingKey : t.key;
            req.token = t.token.empty() ? readConfToken() : t.token;
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
            if (!t.playQueueId.empty()) {
                req.playQueueId = t.playQueueId;
                req.playQueueItemId = t.playQueueItemId;
                req.playQueueVersion = t.playQueueVersion;
                req.containerKey =
                    t.containerKey.empty() ? ("/playQueues/" + t.playQueueId) : t.containerKey;
            }
            if (t.hasSubtitleStreamId) {
                req.hasSubtitleStreamId = true;
                req.subtitleStreamId = t.subtitleStreamId;
            }
            if (t.hasAudioStreamId) {
                req.hasAudioStreamId = true;
                req.audioStreamId = t.audioStreamId;
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
            player.armPlayPendingCue(gen, misterplex::gcast_osd::pendingLoadingLine("Plex"));   // Browse Select: chevron before resolve
            std::fprintf(stderr,
                         "misterplexd: browse play gen=%llu ratingKey=%s source=%s start=%s "
                         "server=%s subtitleStreamID=%s audioStreamID=%s\n",
                         static_cast<unsigned long long>(gen), req.ratingKey.c_str(),
                         t.source.c_str(),
                         req.offsetPresent ? std::to_string(req.offsetMs).c_str() : "pms",
                         req.address.empty() ? "conf" : (req.address + ":" + req.port).c_str(),
                         req.hasSubtitleStreamId ? std::to_string(req.subtitleStreamId).c_str()
                                                 : "default",
                         req.hasAudioStreamId ? std::to_string(req.audioStreamId).c_str() : "default");
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
        if (!castBinding.allow(misterplex::TokenSite::Skip, base, cur.address, cur.token, token, cur.serverMachineId,
                                           misterplex::probePmsIdentity)) {   // #351: skip send site
            std::fprintf(stderr, "misterplexd: %s skip — cast token withheld from unverified server\n", tag);
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
    player.setMarkerAction([&](const misterplex::MediaPlayer::MarkerFire& fire) {
        if (fire.action == misterplex::markers::Action::SeekTo) {
            player.seekMarker(misterplex::markers::adjustSkipTarget(fire.markerEndMs, fire.transcoded));
            return;
        }
        if (fire.action == misterplex::markers::Action::EndPlay) {
            player.stop();
            return;
        }
        if (fire.action != misterplex::markers::Action::PlayNext)
            return;
        // continuing=1 has to be the first stopped timeline. endSession is one-shot.
        pmsTimeline.endSession(fire.mediaMs, fire.durationMs, true);
        if (fire.queued) {
            if (!tryQueueStep(+1, "up-next"))
                std::fprintf(stderr, "misterplexd: up-next queue step failed\n");
            return;
        }
        if (!fire.hasNext || fire.next.key.empty())
            return;
        misterplex::PlayRequest n;
        {
            std::lock_guard<std::mutex> lock(sessionMu);
            n = lastPlay;
        }
        n.key = fire.next.key;
        n.ratingKey = fire.next.ratingKey;
        n.playQueueItemId.clear();
        n.playQueueId.clear();
        n.playQueueVersion.clear();
        n.containerKey.clear();
        n.offsetMs = 0;
        n.offsetPresent = true;
        n.forceLadder = false;
        n.fallbackUsed = false;
        n.pmsLowered = false;
        n.gateDetail.clear();
        n.seekReplay = false;
        n.dispatchGeneration = 0;
        doPlay(n);
    });
    if (spelunkrOn) {
        // P2 11-10: a mid-play NeedsTranscode / SourceError skips the item with a banner (main loop
        // consumes, like EOF). Play thread: never take tvMu here (GBE P1-2).
        player.setSpelunkrPlayFailedHandler([&](const std::string& why) {
            const uint64_t g = spelunkrPlayGen.load(std::memory_order_acquire);
            const int item = spelunkrPlayItemIndex.load(std::memory_order_acquire);
            if (g == 0 || item < 0)
                return false;
            spelunkrFailedSkip.post(g, item, spelunkrPlayFailedSkips.load(std::memory_order_acquire), why);
            return true;
        });
    }
    player.setProgress([&](const std::string& st, int64_t t, int64_t d) {
        const uint64_t generation = activePlayGen.load();
        if (generation != playGen.load())
            return;
        if (gcastOn && generation == castGen.load() && gcastArbiter.dispatchProgress(st, t, d))
            return;   // GCAST: a cast owns this play; never Plex timeline / companion state
        if (st == "ended") {
            if (spelunkrOn) {
                // GH#411: do not drop to Plex auto-next. Do NOT take tvMu here — Stop holds
                // tvMu and joins this play thread (GBE P1-2). Main loop consumes eof gen.
                // GBE N-4: use play-job gen (not startedPlayback) so EOF before first picture advances.
                const uint64_t g = spelunkrPlayGen.load(std::memory_order_acquire);
                spelunkrEofItemIndex.store(spelunkrPlayItemIndex.load(std::memory_order_acquire),
                                           std::memory_order_release);
                spelunkrEofOffsetMs.store(spelunkrPlayOffsetMs.load(std::memory_order_acquire),
                                          std::memory_order_release);
                spelunkrEofPlayedMs.store(t, std::memory_order_release);
                spelunkrEofGen.store(g, std::memory_order_release);
                return;
            }
            pmsTimeline.endSession(t, d);
            if (player.markerSuppressEof()) {
                comp.setState("ended", t, d);
                return;
            }
            // PlexUI TV: hold auto-advance on the Up next card. Cast queues still step here.
            std::string endedKey;
            {
                std::lock_guard<std::mutex> lock(sessionMu);
                endedKey = lastPlay.ratingKey;
            }
            if (player.plexUiArmUpNext(endedKey)) {
                comp.setState("ended", t, d);
                return;
            }
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
        // R1 C1 (FPGA Bug 07:30): the new generation and its PLXH en=1 hold in one step, before
        // the resolve; a release for an older generation can no longer land between them.
        const uint64_t gen = player.r1BeginPlayRequest([&] { return ++playGen; });
        player.armPlayPendingCue(gen, misterplex::gcast_osd::pendingLoadingLine("Plex"));   // playMedia queued: chevron before resolve
        return gen;
    });

    // Keep PMS /:/timeline auth in lock-step with cast-supplied tokens.
    comp.setTokenUpdate([&](const std::string& tok) {
        if (tok.empty())
            return;
        // #355: a control token reaches the live timeline (and lastToken) only through the same gate.
        const std::string tlBase = pmsTimeline.activeBaseUrl();
        std::string mid;
        {
            std::lock_guard<std::mutex> lock(sessionMu);
            mid = lastPlay.serverMachineId;
        }
        if (tlBase.empty() || !castBinding.allow(misterplex::TokenSite::TimelineUpdate, tlBase, "", tok, tok, mid,
                                                 misterplex::probePmsIdentity)) {
            std::fprintf(stderr, "misterplexd: control token withheld (timeline server not verified for it)\n");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(sessionMu);
            lastToken = tok;
        }
        pmsTimeline.updateToken(tok);
    });

    // fr-glass-1032-1025 F1: a playMedia token is the NEW cast's. It feeds lastToken (the next play), never the
    // still-active session's reporter, or the old session's supersede stop/timeline go out with the new
    // server's token and the old server answers 401.
    comp.setPlayMediaToken([&](const std::string& tok) {
        if (tok.empty())
            return;
        std::lock_guard<std::mutex> lock(sessionMu);
        lastToken = tok;
    });

    // Plant lastPlay immediately so skipNext/skipPrevious during async resolve use the
    // new cast's queue bind — not the previous title's lastPlay (P4-SCRUB race).
    if (plexUiOn || spelunkrOn) {
        comp.setPlexUiNavEnabled(true);
        comp.setPlexUiNav([&](const std::string& op) {
            if (spelunkrOn && tvSess) {
                if (op == "up") {
                    player.handlePlaybackInput(misterplex::PlaybackCommand::ChannelUp);
                    return;
                }
                if (op == "down") {
                    player.handlePlaybackInput(misterplex::PlaybackCommand::ChannelDown);
                    return;
                }
                if (op == "select") {
                    player.handlePlaybackInput(misterplex::PlaybackCommand::GuideTune);
                    return;
                }
                if (op == "home" || op == "back") {
                    player.handlePlaybackInput(misterplex::PlaybackCommand::GuideToggle);
                    return;
                }
                return;
            }
            if (!spelunkrOn)
                player.plexUiCompanionNav(op);
        });
    }
    // POST /misterplex/queue/add (behind the companion guard): PMS base + token of the current play session; the PMS
    // owns the queue, the token travels only as a header over no-shell curl (queue_add_pms.hpp).
    comp.setQueueAddTransport(misterplex::qadd::makePmsTransport(
        [&]() {
            std::lock_guard<std::mutex> lock(sessionMu);
            return misterplex::qadd::PmsTarget{lastBase, lastToken};
        },
        misterplex::plexui::makePlexuiHeaderGet(), misterplex::plexui::makePlexuiHttpPut(), machineId));
    comp.setQueueAddNotice([&player](const std::string& t) { player.showOsdNotice(t); });
    // MARK_WATCHED: the playing item, using the same PMS identity /:/timeline
    // already targets (last play, loopback rewrite, IPv6 fallback). A separate
    // request. The timeline reporter is not called. Off: this is not invoked
    // (the menu has no mark rows).
    player.setMarkWatchedFn([&](misterplex::MarkWatchedOp op) {
        if (!misterplex::markWatchedEnabled() || op == misterplex::MarkWatchedOp::None)
            return;
        misterplex::MarkTarget target;
        {
            std::lock_guard<std::mutex> lock(sessionMu);
            target.ratingKey = lastPlay.ratingKey;
            target.token = lastToken;
            target.clientId = machineId.empty() ? "misterplex" : machineId;
            std::string base = misterplex::windowTokenBase(lastBase, castBaseUnverified, defaultPms);
            if (base.empty())
                return;   // #351: no verified base yet; never send the cast token to the conf default
            const std::string lan = !defaultPms.empty() ? defaultPms : base;
            base = misterplex::rewriteLoopbackPmsBase(base, lan);
            if (misterplex::isUnreachableIpv6Host(base) && !defaultPms.empty())
                base = defaultPms;
            // #362: gate the token against the host this request actually hits (selectBase /
            // IPv6 fallback), not the ULA playMedia address.
#ifndef MUT_362_MARK_FALLBACK
            if (!castBinding.allow(misterplex::TokenSite::MarkWatched, base, "", lastToken, lastToken,
                                   lastPlay.serverMachineId, misterplex::probePmsIdentity))
                return;
#endif
            target.baseUrl = std::move(base);
        }
        std::thread([&player, op, target] {
            try {
                const auto get = misterplex::plexui::makePlexuiHeaderGet();
                const auto put = misterplex::plexui::makePlexuiHttpPut();
                auto transport = [&](const misterplex::MarkHttpRequest& req) {
                    misterplex::MarkTransportResult tr;
                    misterplex::BrowseCancelFn cancel;
                    if (req.method == "PUT") {
                        const auto r = put(req.url, "", "", req.headers, 4000, 8000, cancel);
                        tr.status = r.cancelled ? 0 : r.status;
                        return tr;
                    }
                    const auto r = get(req.url, req.headers, 4000, 8000, cancel);
                    tr.status = r.cancelled ? 0 : r.status;
                    return tr;
                };
                const misterplex::MarkApplyResult applied =
                    misterplex::applyMarkWatched(transport, op, target);
                std::fprintf(stderr, "misterplexd: mark op=%d ratingKey=%s http=%d\n",
                             static_cast<int>(op), target.ratingKey.c_str(), applied.httpStatus);
                player.showOsdNotice(applied.osd.empty() ? "MARK REFUSED" : applied.osd);
            } catch (...) {
                std::fprintf(stderr, "misterplexd: mark failed\n");
                player.showOsdNotice("MARK REFUSED");
            }
        }).detach();
    });
    // Self-update (frontier item 6). OFF unless conf UPDATE_ENABLE is "1" + an https UPDATE_URL AND
    // keyUsable(MPX_UPDATE_PUBKEY_HEX). This tip bakes the glass public key, so that key gate is open.
    // UPDATE_ENABLE still defaults off: the phone page stays "not available" until misterplex.conf sets it.
    // The all-zero 64-hex string is still the placeholder and fails closed. Nothing is fetched at boot; recovery
    // only completes or rolls back an interrupted earlier swap. No key, URL or body is ever logged.
    static mpx::upd::RealFs updFs;
    std::unique_ptr<mpx::upd::Updater> updater;
    {
#ifdef MPX_STATUS_VERSION
        const std::string curVersion = MPX_STATUS_VERSION;
#else
        const std::string curVersion = "dev";
#endif
        std::map<std::string, std::string> uc;
        for (const char* k : {"UPDATE_ENABLE", "UPDATE_URL", "UPDATE_ALLOW_AUX"}) uc[k] = loadConf(confPath, k);
        mpx::upd::Paths up;
        {
            char exe[1024];
            const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof exe - 1);
            up.daemon = n > 0 ? std::string(exe, static_cast<size_t>(n)) : std::string();
        }
        up.journal = up.daemon + ".update.journal";
        up.marker = "/tmp/misterplexd-update.healthy";
        const bool keyOk = mpx::upd::keyUsable(MPX_UPDATE_PUBKEY_HEX);
        if (!up.daemon.empty()) {
            const mpx::upd::Boot ub = mpx::upd::recoverOnBoot(updFs, up);
            if (ub != mpx::upd::Boot::Normal)
                std::fprintf(stderr, "misterplexd: update recovery state=%d\n", static_cast<int>(ub));
            if (ub == mpx::upd::Boot::RunPending) {  // first run after a swap: mark healthy once it has stayed up
                std::thread([up, curVersion] {
                    std::this_thread::sleep_for(std::chrono::seconds(30));
                    mpx::upd::commitHealthy(updFs, up, curVersion);
                }).detach();
            }
        }
        mpx::upd::Ctx uctx;
        uctx.conf = mpx::upd::parseUpdateConf(uc);
        uctx.current = curVersion;
        uctx.playing = [&player] { return player.playing(); };
        uctx.verify = mpx::upd::makeUpdateVerifier();
        uctx.fetch = mpx::upd::makeHttpsFetch([](const std::string& url, uint64_t maxBytes) {
            misterplex::TlsVerifiedRequest rq;
            rq.url = url;
            rq.maxBody = static_cast<size_t>(maxBytes) + 1;
            rq.timeoutSec = 30;
            rq.followRedirects = false;  // an artifact must come from the manifest's own host
            const misterplex::TlsVerifiedResult rs = misterplex::tlsVerifiedGet(rq);
            mpx::upd::HttpGetResult out;
            out.ok = rs.transportOk;
            out.status = rs.status;
            out.body = rs.body;
            return out;
        });
        updater.reset(new mpx::upd::Updater(updFs, up, uctx, keyOk && !up.daemon.empty(), [](const std::string& m) {
            std::fprintf(stderr, "misterplexd: %s\n", m.c_str());
        }));
        std::fprintf(stderr, "misterplexd: update: %s\n", updater->enabled() ? "on" : "off");
        if (updater->enabled()) {
            mpx::upd::Updater* u = updater.get();
            // The phone page's one-tap POST calls this. False while a run is already in progress.
            // The status refresh below does not call it (no silent and no timer check).
            comp.setUpdateStart([u] { return u->start(); });
        }
    }
    // phonestatus provider. The page the companion serves is phone_status_http (issue #57): "not available"
    // until setUpdateStart above is registered, and the one-tap POST calls that starter (issue #59). This provider
    // is not the served page. Static facts only: no token, no PMS URL. "not available" unless update is on and the
    // build holds a real key. Real facts (frontier item 3, status_facts.hpp): md5 of this running binary (hashed once),
    // the playback title, the helper state. Version/commit stay "dev"/"unknown" unless the build defines
    // MPX_STATUS_VERSION / MPX_STATUS_COMMIT.
    comp.setStatusProvider([&player, &autoPictureMode, &lastAutoPic, &updater]() {
        misterplex::phonestatus::Facts f;
#ifdef MPX_STATUS_VERSION
        f.version = MPX_STATUS_VERSION;
#endif
#ifdef MPX_STATUS_COMMIT
        f.commit = MPX_STATUS_COMMIT;
#endif
        f.title = [&player]() { return player.playbackTitle(); };
        f.helper = []() -> const char* {
            const std::string remote = misterplex::gcast::helperBaseFromEnv();
            const bool remoteOk = !remote.empty() && misterplex::gcast::probeHttpHealth(remote, 400);
            const bool localOk = remote.empty() && misterplex::gcast::cachedLoopbackHelperHealth();
            return misterplex::phonestatus::helperText(!remote.empty(), remoteOk, localOk);
        };
        static const std::string exeMd5 =   // the binary does not change under a running process; thread-safe once
            misterplex::phonestatus::md5OrUnknown(f.exePath.c_str());
        misterplex::phonestatus::Status st = misterplex::phonestatus::build(f, exeMd5);
        char idBuf[16];
        std::snprintf(idBuf, sizeof idBuf, "%08x", static_cast<unsigned>(misterplex::CoreGate::process().buildId()));
        st.update = !updater || updater->state() == mpx::upd::WireState::Off ? misterplex::phonestatus::UpdateState::Unavailable
                    : updater->state() == mpx::upd::WireState::Running ? misterplex::phonestatus::UpdateState::Busy
                                                                      : misterplex::phonestatus::UpdateState::Checkable;
        st.autoPicture = lastAutoPic.text(misterplex::autopic::adviseEnabled(autoPictureMode));
        st.coreId = idBuf;
        st.pairedRaster = misterplex::CoreGate::process().pairedRaster();
        return st;
    });

    comp.setPlay([&](const misterplex::PlayRequest& req) {
        {
            std::lock_guard<std::mutex> lock(sessionMu);
            lastPlay = req;
            if (!req.token.empty())
                lastToken = req.token;
            // Prefer cast address as provisional base when present.
            // address=host:port plus port= must not become host:port:port.
            if (!req.address.empty()) {
                // #347: cast host only. #351: IPv6 literal -> empty until the machine-id-verified commit.
                lastBase = misterplex::provisionalCastBase(
                    req.address, req.port, req.protocol, defaultPms, comp.localLanIp());
                castBaseUnverified = lastBase.empty();
            } else {
                // #351/#355: a refused/cleared cast target must not keep the previous cast's base, seek
                // session or stored token (the new token would go there).
                misterplex::stageCastWithoutTarget(req, lastBase, lastToken, castBaseUnverified, seekReuse);
            }
        }
        doPlay(req);
    });

    remoteDirectReplay = doPlay;
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
            //  lab scrub black screen: a seek replays the SAME item on the SAME server, so it
            // resolves with the token that started it (playMedia token=), never a controller
            // token picked up from the seek request (see castControlRefreshToken).
            // #351 glass f865f5a: and the SAME server (lastPlay's address/id); a refused cast has none and
            // no caster token, so the seek never borrows the previous cast's host.
            if (!m.key.empty() && lastPlay.key == m.key && m.token != lastPlay.token)
                std::fprintf(stderr, "misterplexd: seek keeps the playMedia server and token\n");
            misterplex::pinSeekServer(m, lastPlay);
        }
        m.seekReplay = true;
        doPlay(m);
    };
    // OWENB-P1B T5: the cast volume (setParameters) -> the player (OB: the core's attenuation).
    comp.setVolume([&player](int volume) { player.setVolumePercent(volume); });
    comp.setOnStreams([&player](const misterplex::SetStreamsRequest& req) {
        return player.applyUserStreams(req);
    });
    player.setOpenedStreamsHandler([&comp, &pmsTimeline](int audio, int sub) {
        comp.noteStreams(audio, sub);
        pmsTimeline.noteStreams(audio, sub);
    });
    misterplex::wirePlaybackControls(comp, player, pmsTimeline, transport, activePlayGen,
        playHandoffMu, seekMu, seekStart, [&](uint64_t stopGen) {
            // R1 C1: the STOP ends every PLAY request before its own generation; a PLAY (and a seek)
            // after the STOP keeps its hold (FPGA Bug 08:17 (1)).
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
    // Empty / unbound queue = no-op log. SPELUNKR: next channel (GH#411).
    if (spelunkrOn)
        comp.setSkipWhenStopped(true);
    comp.setSkipNext([&]() {
        if (spelunkrOn) {
            if (tvSess)
                player.handlePlaybackInput(misterplex::PlaybackCommand::ChannelUp);
            return;  // GH#411: never Plex play-queue skipNext in TV mode
        }
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
        if (spelunkrOn) {
            if (tvSess)
                player.handlePlaybackInput(misterplex::PlaybackCommand::ChannelDown);
            return;  // GH#411: never Plex play-queue skipPrevious in TV mode
        }
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

#ifdef MPX_GIT_COMMIT
    const char* phoneCommit = MPX_GIT_COMMIT[0] ? MPX_GIT_COMMIT : "unknown";
#else
    const char* phoneCommit = "unknown";
#endif
    {
        const std::string phoneMd5 = misterplex::phone_status::selfExeMd5();
        comp.setPhoneIdentity("0.4.1", phoneCommit, phoneMd5);
        misterplex::phone_status::Facts id;
        id.version = "0.4.1";
        id.commit = phoneCommit;
        id.md5 = phoneMd5;
        id.mode = "stopped";
        std::fprintf(stderr, "misterplexd: %s\n",
                     misterplex::phone_status::statusLogLine(id).c_str());
    }

    auto quiesceSpelunkr = [&]() {
        if (!spelunkrOn)
            return;
        {
            std::lock_guard<std::recursive_mutex> lock(tvMu);
            tvStopping = true;
        }
        player.setSpelunkrCmd({});
        if (guideThr.joinable())
            guideThr.join();
        // Join without tvMu: the job may still be checking its tune generation.
        // No play() can follow retirement, even if it passed its last cancel check.
        playQ.shutdown();
    };
    if (!comp.start()) {
        std::fprintf(stderr, "misterplexd: companion start failed\n");
        quiesceSpelunkr();
        if (spelunkrOn)
            player.shutdown();
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
                // #347: no conf-default contact while a cast from another server plays.
                const bool playActive = comp.wantPlay();   // read outside sessionMu (no lock nesting)
                std::string activeBase;
                {
                    std::lock_guard<std::mutex> lock(sessionMu);
                    activeBase = lastBase;
                }
                const bool allowed = misterplex::heartbeatProbeAllowed(playActive, activeBase, defaultPms);
                if (allowed)
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
            ytLiveSession.store(false);
            misterplex::gcast::yt_live::liveRelay().stop();
            ytFe.ended(false);
        });
        gcastArbiter.setProgress(Owner::YouTube, [&](const std::string& st, int64_t t, int64_t d) {
            const int64_t loungeDur = misterplex::gcast::yt_live::liveLoungeDurationMs(ytLiveSession.load(), d);
            if (st == "stopped" && player.reopenPending()) {   // bounded reopen restarts the same cast
                ytFe.report("buffering", t, loungeDur);
                return;
            }
            if (st == "ended" || st == "stopped") {
                ytLiveSession.store(false);
                misterplex::gcast::yt_live::liveRelay().stop();
                gcastArbiter.release(Owner::YouTube, castToken.load());
                ytFe.ended(st == "ended");   // natural end may auto-advance the Lounge playlist
                return;
            }
            ytFe.report(st, t, loungeDur);
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
        {
            const std::string mirrorConf = loadConf(confPath, "AIRPLAY_MIRROR");
            apCfg.mirror = misterplex::gcast::airplayMirrorWanted(std::getenv("AIRPLAY_MIRROR"),
                                                                  mirrorConf.c_str());
            if (apCfg.mirror)
                std::fprintf(stderr, "misterplexd: AIRPLAY_MIRROR=1 (SETUP type 110 accepted; FPGA feed opt-in)\n");
        }
        if (apCfg.mirror) {
            if (auto* prod = player.owenbRingProducer()) {
                player.setAirplayMirrorProducer(prod);
                apCfg.mirrorProducer = prod;
                std::fprintf(stderr, "misterplexd: AIRPLAY_MIRROR decoder feed attached (OwenbRing)\n");
            } else {
                std::fprintf(stderr, "misterplexd: AIRPLAY_MIRROR ring not open yet; will attach on first session\n");
            }
            apCfg.onMirrorAu = [&](const uint8_t* p, size_t n, int64_t) {
                return player.pushAirplayMirrorAu(p, n, -1);   // kNoPts90k: picture first
            };
            apCfg.onMirrorPcm = [&](const uint8_t* p, size_t n) { return player.pushAirplayMirrorPcm(p, n); };
        }
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
                if (gcastArbiter.owner() == Owner::AirPlay &&
                    misterplex::gcast::rateEventAllowed(apHandoffHeldGen.load() != 0 ||
                                     (apMirrorHeldPlex.load() && !player.airplayMirrorActive()))) {
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
            case ApEventType::Notice:
                // Out-of-spec video: the toast, then the ready card. Playback was not started.
                player.showIdleNotice(e.url);
                airplayFe.snapshot().ended();
                break;
            case ApEventType::MirrorStart: {
                const uint64_t tok = gcastArbiter.claim(Owner::AirPlay);
                castToken.store(tok);
                if (misterplex::gcast::mirrorStartAction(player.playing() && !player.airplayMirrorActive()) ==
                    misterplex::gcast::MirrorStartAction::HoldPlex) {
                    // Keep the last Plex frame (paused) until the first mirror AU begins the ring session.
                    std::lock_guard<std::mutex> handoff(playHandoffMu);
                    ++playGen;   // the held play's progress is dropped; an in-flight doPlay aborts
                    player.pause();
                    apMirrorHeldPlex.store(true);
                    std::fprintf(stderr, "misterplexd: airplay mirror start: Plex frame held until the first AU\n");
                    break;
                }
                if (!player.beginAirplayMirrorSession())
                    std::fprintf(stderr,
                                 "misterplexd: airplay mirror session begin failed (decoder feed not attached)\n");
                break;
            }
            case ApEventType::MirrorStop: {
                const bool began = player.airplayMirrorActive();
                player.endAirplayMirrorSession();
                if (apMirrorHeldPlex.exchange(false) && misterplex::gcast::mirrorStopNeedsPlexStop(true, began)) {
                    std::lock_guard<std::mutex> handoff(playHandoffMu);
                    misterplex::retirePlayback(player, pmsTimeline);   // no mirror frame ever came: idle card
                }
                gcastArbiter.release(Owner::AirPlay, castToken.load());
                airplayFe.snapshot().ended();
                break;
            }
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
                              c.kind == YtCmd::Seek ? c.fromMs : -1, c.kind == YtCmd::Seek)) {
                    ytFe.ended(false);
                    break;
                }
                // Seek keeps the cues and moves them onto this play's epoch.
                // A new id drops them. The clock only paints a matching epoch,
                // so a Plex or AirPlay play does not show these lines.
                if (misterplex::gcast::lounge::ytCaptionsEnabled())
                    misterplex::gcast::yt_captions::board().notePlay(
                        player.playbackEpoch(), c.videoId, c.kind == YtCmd::Seek);
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
                if (misterplex::gcast::lounge::ytCaptionsEnabled())
                    misterplex::gcast::yt_captions::board().clear();
                castStop(Owner::YouTube);
                break;
            case YtCmd::Volume:
                // Queued only when YT_VOLUME is on and the lounge arg parsed as 0-100.
                if (misterplex::gcast::lounge::ytVolumeEnabled() && c.volume >= 0 && c.volume <= 100)
                    player.setVolumePercent(c.volume);
                break;
            case YtCmd::Captions: {
                // Queued only when YT_CAPTIONS is on. Hide clears the band.
                // Show asks yt-dlp for the signed subtitle file off this thread
                // so pause and stop are not stuck behind the extract.
                // Unsigned timedtext is not fetched (that GET is HTTP 429).
                if (!misterplex::gcast::lounge::ytCaptionsEnabled())
                    break;
                if (!c.captionShow || c.captionLang.empty() || c.videoId.empty() ||
                    gcastArbiter.owner() != Owner::YouTube) {
                    misterplex::gcast::yt_captions::board().hide();
                    std::fprintf(stderr, "misterplexd: yt captions hidden\n");
                    break;
                }
                misterplex::gcast::yt_captions::YtCaptionRequest req;
                req.videoId = c.videoId;
                req.lang = c.captionLang;
                req.kind = c.captionKind;
                int pyMaj = 0, pyMin = 0;
                misterplex::gcast::yt_dlp::imagePython(pyMaj, pyMin);
                std::string ytdlpPref;
                if (const char* e = std::getenv("MPX_YTDLP_BIN"); e && *e)
                    ytdlpPref = e;
                else
                    ytdlpPref = loadConf(confPath, "YTDLP_BIN");
                const misterplex::gcast::yt_dlp::FoundBinary ytdlpBin = misterplex::gcast::yt_dlp::findBinary(
                    pyMaj, pyMin, misterplex::gcast::yt_dlp::candidatePaths(),
                    ytdlpPref.empty() ? nullptr : ytdlpPref.c_str());
                const bool zipapp = ytdlpBin.kind == misterplex::gcast::yt_dlp::ReleaseKind::Zipapp;
                const std::string ytdlpPath = ytdlpBin.path;
                // Cap detached caption loads. A superseded load exits before it
                // spawns yt-dlp (isCurrent), so the cap is rarely reached.
                static std::atomic<int> capInflight{0};
                const uint64_t gen = misterplex::gcast::yt_captions::board().beginLoad(req.videoId);
                if (capInflight.load() >= 3) {
                    // Gen-scoped: drops only this load; never hides a newer one.
                    misterplex::gcast::yt_captions::board().failLoad(gen);
                    std::fprintf(stderr, "misterplexd: yt captions busy, dropped video=%s\n", req.videoId.c_str());
                    break;
                }
                capInflight.fetch_add(1);
                std::thread([gen, req, ytdlpPath, zipapp] {
                    struct Done {
                        ~Done() { capInflight.fetch_sub(1); }
                    } done;
                    std::string why;
                    const misterplex::CueList cues = misterplex::gcast::yt_captions::loadYtCaptionCues(
                        req, [&](const std::vector<std::string>& plan) {
                            if (!misterplex::gcast::yt_captions::captionArgsAreYtdlp(plan) || ytdlpPath.empty())
                                return std::string();
                            return misterplex::gcast::yt_dlp::resolveCaptionBody(
                                ytdlpPath, zipapp, "python3", req,
                                [gen] { return misterplex::gcast::yt_captions::board().isCurrent(gen); }, &why);
                        });
                    if (cues.cues.empty()) {
                        std::fprintf(stderr, "misterplexd: yt captions: no cues video=%s lang=%s via=ytdlp why=%s\n",
                                     req.videoId.c_str(), req.lang.c_str(), why.empty() ? "no-file" : why.c_str());
                        return;
                    }
                    if (!misterplex::gcast::yt_captions::board().finishLoad(gen, req.videoId, cues))
                        return;
                    std::fprintf(stderr, "misterplexd: yt captions armed video=%s lang=%s kind=%s cues=%zu via=ytdlp\n",
                                 req.videoId.c_str(), req.lang.c_str(), req.kind.empty() ? "-" : req.kind.c_str(),
                                 cues.cues.size());
                }).detach();
                break;
            }
            }
        });
    }

    // GH#8: AirPlay stays on the ready card when a helper is up. Failed/404
    // /probe is capabilities unknown; fall back to /health, then on-card :18781.
    auto airplayReady = [&]() {
        if (!gcastApUp)
            return false;
        const std::string remote = misterplex::gcast::helperBaseFromEnv();
        const bool loopHealth = misterplex::gcast::cachedLoopbackHelperHealth();
        const bool remoteSet = !remote.empty();
        const bool remoteProbe = remoteSet && misterplex::gcast::cachedHelperProbeSelfTest(remote);
        const bool remoteHealth = remoteSet && misterplex::gcast::cachedHelperHealth(remote);
        const bool loopProbe =
            !remoteSet && loopHealth &&
            misterplex::gcast::cachedHelperProbeSelfTest(misterplex::gcast::kLoopbackHelperBase);
        const bool ok = misterplex::gcast::airplayAdvertiseReady(true, remoteSet, remoteProbe, remoteHealth,
                                                                loopHealth, loopProbe);
        static std::string lastPick;
        const char* pick = misterplex::gcast::airplayHelperPick(remoteSet, remoteProbe, remoteHealth, loopHealth);
        if (lastPick != pick) {
            std::fprintf(stderr, "misterplexd: airplay helper %s (probe=%d health=%d loopback=%d)\n", pick,
                         remoteProbe ? 1 : 0, remoteHealth ? 1 : 0, loopHealth ? 1 : 0);
            lastPick = pick;
        }
        return ok;
    };
    bool gcastApShown = false;
    std::string shownLinkCode;
    bool loggedReady = false;
    auto publishReady = [&](bool ap, bool yt) {
        if (spelunkrOn) {
            player.setIdleReadyLine("Spelunkr: Start / X / F1 for guide; A / Enter to tune");
            return;
        }
        const std::string code = misterplex::gcast_osd::gcastLinkCodeForIdle(
            misterplex::plexui::plexAccountTokenOk(plexTvToken), comp.plexTvCode());
        const std::string base = misterplex::gcast_osd::gcastReadyLine(ap, yt);
        const std::string ready = misterplex::gcast_osd::gcastIdleHint(ap, yt, code);
        player.setIdleReadyLine(ready);
        if (!loggedReady || code != shownLinkCode) {
            std::fprintf(stderr, "misterplexd: GCAST idle ready line \"%s\"%s\n", misterplex::logUrl(base).c_str(),
                         misterplex::gcast_osd::gcastLinkLogSuffix(code));
            shownLinkCode = code;
            loggedReady = true;
        }
    };
    if (gcastOn) {   // P4: "Ready · AirPlay · YouTube" on the idle screens (flags off: "Ready")
        // YouTube stays off this line until Lounge connects (gcastYtUp updated below).
        // KI-5: when AirPlay is on, the hook restarts the helper with the daemon.
        // Hold the first Ready line (bounded) until /probe passes, so the card
        // is not published as "Ready · YouTube" and then patched a few seconds later.
        if (gcastAirplay) {
            const auto waitStart = std::chrono::steady_clock::now();
            for (;;) {
                gcastApShown = airplayReady();
                const int elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                          std::chrono::steady_clock::now() - waitStart)
                                                          .count());
                const int delay = misterplex::gcast::airplayFirstReadyDelayMs(gcastApShown, elapsed);
                if (delay <= 0 || g_stop.load())
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
        } else {
            gcastApShown = airplayReady();
        }
        gcastApReady.store(gcastApShown);
        publishReady(gcastApShown, gcastYtUp.load());
    }
    // Issue #57: refresh the phone status page from the core, the playback title, and the helper
    // probe the daemon already uses. The helper word is up or down. The address is not copied
    // onto the page. The probe is not on the HTTP thread, and not on every idle tick.
    // This refresh does not start self-update (issue #59: never silent, never a timer check).
    // The one-tap POST /misterplex/update calls updateStart_ when updates are on. newer stays
    // unknown until that run reports not-newer or swapped.
    auto publishPhone = [&](bool helperToo) {
        const uint32_t id = misterplex::CoreGate::process().buildId();
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%08X", id);
        std::string pairing = "closed";
        if (misterplex::CoreGate::process().idleErrorPaintAllowed()) {
            pairing = misterplex::CoreGate::process().pairingErrorText();
            if (pairing.empty())
                pairing = "incorrect pairing";
        } else if (misterplex::CoreGate::process().open()) {
            const std::string raster = misterplex::CoreGate::process().pairedRaster();
            pairing = raster.empty() ? "paired" : "paired " + raster;
        }
        comp.notePhoneCore(hex, pairing);
        comp.notePhoneTitle(player.playbackTitle());
        const char* newer = "unknown";
        if (updater) {
            const mpx::upd::Result last = updater->last();
            if (last == mpx::upd::Result::Swapped)
                newer = "yes";
            else if (last == mpx::upd::Result::NotNewer)
                newer = "no";
        }
        comp.notePhoneNewer(newer);
        if (!helperToo)
            return;
        const std::string remote = misterplex::gcast::helperBaseFromEnv();
        const bool remoteSet = !remote.empty();
        const bool helperUp = misterplex::gcast::airplayAdvertiseReady(
            true, remoteSet, remoteSet && misterplex::gcast::cachedHelperProbeSelfTest(remote),
            remoteSet && misterplex::gcast::cachedHelperHealth(remote),
            misterplex::gcast::cachedLoopbackHelperHealth(), false);
        comp.notePhoneHelper(helperUp ? "up" : "down");
    };
    publishPhone(true);
    auto nextPhoneHelper = Clock::now() + std::chrono::seconds(2);
    std::atomic<bool> ambientBusy{false};
    std::atomic<int64_t> ambientFetchedAt{0};
    std::atomic<int> ambientCards{0};
    std::thread ambientThr;
    auto ambientNowMs = []() -> int64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    bool tvCoreOpen = false;
    bool tvResumeArmed = false;   // GBE #623 frame 153: resume chrome armed while the core is closed
    uint64_t tvResumeGeneration = 0;
    bool serviceReadyWritten = false;
    pid_t serviceChild = -1;
    player.setServiceSwitchHandler([&](bool target) {
        const int logFd = ::open("/tmp/misterplex-service-switch.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (logFd < 0) {
            std::fprintf(stderr, "misterplexd: Service switch log open failed: %s\n", std::strerror(errno));
            return false;
        }
        const long maxFd = ::sysconf(_SC_OPEN_MAX);
        if (maxFd < 0) {
            ::close(logFd);
            std::fprintf(stderr, "misterplexd: Service switch fd limit unavailable\n");
            return false;
        }
        ytWarm.stop(); // The service-switch script must not overlap a resident resolver.
        const pid_t child = ::fork();
        if (child == 0) {
            ::setsid();
            ::dup2(logFd, STDOUT_FILENO);
            ::dup2(logFd, STDERR_FILENO);
            for (int fd = 3; fd < maxFd; ++fd)
                ::close(fd);
            ::execl("/bin/sh", "sh", "/media/fat/misterplex/bin/misterplex_mode.sh",
                    target ? "--osd-spelunkr" : "--osd-plex", static_cast<char*>(nullptr));
            ::_exit(127);
        }
        ::close(logFd);
        if (child < 0) {
            std::fprintf(stderr, "misterplexd: Service switch fork failed: %s\n", std::strerror(errno));
            return false;
        }
        // Only an accepted operator switch releases the listen's TERM hold.
        g_signalHold.store(false, std::memory_order_release);
        serviceChild = child;
        return true;
    });
    while (!g_stop.load()) {
        if (serviceChild > 0) {
            int status = 0;
            if (::waitpid(serviceChild, &status, WNOHANG) == serviceChild) {
                if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                    const auto state = misterplex::service::readState();
                    if (!misterplex::service::writeState({"error", state.target}))
                        std::fprintf(stderr, "misterplexd: Service child failure state write failed\n");
                    std::fprintf(stderr, "misterplexd: Service switch child failed status=%d\n", status);
                }
                serviceChild = -1;
            }
        }
        const bool helperToo = Clock::now() >= nextPhoneHelper;
        publishPhone(helperToo);
        if (helperToo)
            nextPhoneHelper = Clock::now() + std::chrono::seconds(2);
        const bool ytNow = gcastYt && ytFe.loungeConnected();
        const bool apNow = airplayReady();
        const std::string codeNow = comp.plexTvCode();
        if (ytNow != gcastYtUp.load() || apNow != gcastApShown || codeNow != shownLinkCode) {
            gcastYtUp.store(ytNow);
            gcastApShown = apNow;
            gcastApReady.store(gcastApShown);
            publishReady(gcastApShown, gcastYtUp.load());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (g_termHeld.load(std::memory_order_acquire) &&
            !g_termHoldLogged.exchange(true, std::memory_order_acq_rel))
            std::fprintf(stderr,
                         "misterplexd: SIGTERM ignored; companion listen still owns :3005\n");
        if (spelunkrOn && tvSess) {
            // GBE #623 frame 153: arm the resume's tuning chrome BEFORE coreGateTick can open the gate,
            // so owenbOpened sees pending (keepChrome) and never paints SPELUNKR / OPEN THE GUIDE first.
            std::lock_guard<std::recursive_mutex> lock(tvMu);
            const auto rc = misterplex::spelunkr::resumeChromeToArm(*tvSess, lastPlayedChannel, tvCoreOpen,
                                                                  tvHadCoreOpenOnce, tvResumeArmed,
                                                                  player.spelunkrUiPending());
            if (rc.arm) {
                player.beginSpelunkrTune(0, rc.dial, rc.name);
                tvResumeArmed = true;
            }
        }
        player.coreGateTick();
        const auto& ytGate = misterplex::CoreGate::process();
        if (!spelunkrOn && misterplex::envFlagWanted("GYT") && serviceChild < 0 &&
            !g_stop.load() && !g_termHeld.load() && (ytGate.owenbOpen() || ytGate.open()))
            ytWarm.startAsync();
        else
            ytWarm.stop();
        player.serviceTick();
        if (spelunkrOn && tvSess) {
            const bool open = misterplex::CoreGate::process().owenbOpen();
            if (open && !tvCoreOpen) {
              std::lock_guard<std::recursive_mutex> lock(tvMu);
              tvResumeArmed = false;
              // GBE #623 frame 153: compute the resume FIRST; land only when it is empty.
              // Cold boot: preferCurrent=false so startup selectIndex(primetime) cannot
              // force CH 8 over SPELUNKR_LAST_CHANNEL / logo landing (GBE P1-1).
              const bool preferCurrent = tvHadCoreOpenOnce;
              const auto resume = preferCurrent
                  ? misterplex::spelunkr::resumeCurrentOrLast(*tvSess, lastPlayedChannel, true)
                  : misterplex::spelunkr::resumeLastChannel(*tvSess, lastPlayedChannel);
              tvResumeGeneration = resume.generation;
              if (resume.generation != 0) {
                  std::string resumeName;
                  if (resume.idx >= 0 &&
                      resume.idx < static_cast<int>(tvSess->lineup.channels.size()))
                      resumeName = tvSess->lineup.channels[static_cast<size_t>(resume.idx)].name;
                  player.beginSpelunkrTune(resume.generation, "CH " + std::to_string(resume.dial),
                                           resumeName);
                  playQ.post(resume);
                  tvHadCoreOpenOnce = true;  // GBE N-3: only after a real tune posts
              } else {
                  // empty resume → landing (Open the Guide); no auto-tune. Clears any pre-armed chrome.
                  player.showSpelunkrLanding();
              }
              publishGuide();
            }
            {
                std::lock_guard<std::recursive_mutex> lock(tvMu);
                // GBE #620 second review P2 / #621 P2-a, P3-a: a real picture of the current play, held for
                // kSkipBudgetResetPlayMs, resets the shared consecutive skip budget (watchdog + failed-item
                // chains). A picture that then fails (late NeedsTranscode) keeps counting toward the landing.
                const uint64_t budgetGen = spelunkrPlayGen.load(std::memory_order_acquire);
                if (spelunkrBudgetReset.poll(budgetGen, open, tvStopping, player.playing(),
                                             player.owenbDecodedPicture(), tvSess->tv.current(budgetGen),
                                             zapSteadyMs())) {
                    std::fprintf(stderr,
                                 "misterplexd: spelunkr: skip budget reset after %lld ms of picture item=%d "
                                 "prior=%u gen=%llu\n",
                                 static_cast<long long>(misterplex::spelunkr::kSkipBudgetResetPlayMs),
                                 spelunkrPlayItemIndex.load(std::memory_order_acquire),
                                 spelunkrPlayFailedSkips.load(std::memory_order_acquire),
                                 static_cast<unsigned long long>(budgetGen));
                    spelunkrPlayFailedSkips.store(0u, std::memory_order_release);
                }
                auto recovery = spelunkrFirstPicture.poll(
                    *tvSess, zapSteadyMs(), open && !tvStopping && !g_stop.load() && player.playing(),
                    player.owenbDecodedPicture(), spelunkrPlayItemIndex.load(std::memory_order_acquire));
                if (recovery.generation != 0) {
                    const auto& channel = tvSess->lineup.channels[static_cast<size_t>(recovery.idx)];
                    player.beginSpelunkrTune(recovery.generation, "CH " + std::to_string(recovery.dial),
                                             channel.name);
                    std::fprintf(stderr,
                                 "misterplexd: spelunkr: first-picture watchdog: RC_PIC unchanged after %lld ms; "
                                 "skip same channel dial=%d item=%d attempt=%u gen=%llu\n",
                                 static_cast<long long>(misterplex::spelunkr::kFirstPictureTimeoutMs),
                                 recovery.dial, recovery.endedItemIndex, recovery.firstPictureAttempt,
                                 static_cast<unsigned long long>(recovery.generation));
                    playQ.post(std::move(recovery));
                }
            }
            // Consume EOF handoff off the play thread (GBE P1-2 / P2-2).
            if (const uint64_t eofG = spelunkrEofGen.exchange(0)) {
              std::lock_guard<std::recursive_mutex> lock(tvMu);
              if (!tvStopping && tvSess && tvSess->tv.current(eofG)) {
                const int idx = tvSess->tv.index();
                if (idx >= 0 && idx < static_cast<int>(tvSess->lineup.channels.size())) {
                  auto r = misterplex::spelunkr::advanceAfterEof(
                      *tvSess, eofG, spelunkrEofItemIndex.exchange(-1),
                      spelunkrEofOffsetMs.exchange(-1), spelunkrEofPlayedMs.exchange(-1));
                  if (r.generation != 0) {
                    std::string nm = tvSess->lineup.channels[static_cast<size_t>(idx)].name;
                    player.beginSpelunkrTune(r.generation, "CH " + std::to_string(r.dial), nm);
                    std::fprintf(stderr,
                                 "misterplexd: spelunkr: eof next-item idx=%d id=%s dial=%d gen=%llu\n",
                                 idx, r.id.c_str(), r.dial,
                                 static_cast<unsigned long long>(r.generation));
                    playQ.post(std::move(r));
                  }
                }
              } else {
                std::fprintf(stderr, "misterplexd: spelunkr: stale eof dropped gen=%llu\n",
                             static_cast<unsigned long long>(eofG));
              }
            }
            // P2 11-10: mid-play failure / advance timeout -> skip the item with a banner, held picture.
            if (auto fs = spelunkrFailedSkip.take(); fs.generation != 0) {
              std::lock_guard<std::recursive_mutex> lock(tvMu);
              if (!tvStopping && open && tvSess && tvSess->tv.current(fs.generation)) {
                const int idx = tvSess->tv.index();
                if (idx >= 0 && idx < static_cast<int>(tvSess->lineup.channels.size())) {
                  const std::string nm = tvSess->lineup.channels[static_cast<size_t>(idx)].name;
                  auto r = misterplex::spelunkr::advanceAfterFailedItem(*tvSess, fs.generation, fs.itemIndex,
                                                                        fs.failedSkips);
                  if (r.generation != 0) {
                    player.beginSpelunkrTune(r.generation, "CH " + std::to_string(r.dial), nm);
                    player.showSpelunkrBanner("CH " + std::to_string(r.dial), nm, fs.why, true);
                    std::fprintf(stderr,
                                 "misterplexd: spelunkr: failed item skip why=%s item=%d skips=%u gen=%llu\n",
                                 fs.why.c_str(), fs.itemIndex, fs.failedSkips,
                                 static_cast<unsigned long long>(r.generation));
                    playQ.post(std::move(r));
                  } else {
                    // GBE #620 P2-a: never land the guide from the handoff; the job owns terminals.
                    std::fprintf(stderr,
                                 "misterplexd: spelunkr: failed-item skip dropped (no request) why=%s item=%d "
                                 "skips=%u gen=%llu\n",
                                 fs.why.c_str(), fs.itemIndex, fs.failedSkips,
                                 static_cast<unsigned long long>(fs.generation));
                    // GBE #623: the failed play's end holds the frame + tuning banner (no idle paint), and
                    // its job already returned, so this generation has no other terminal. Land with the
                    // job's own no-next text instead of leaving the banner up forever.
                    player.showSpelunkrError("SKIP FAILED: NO NEXT", true);
                  }
                }
              } else {
                std::fprintf(stderr, "misterplexd: spelunkr: stale failed-item skip dropped (stale gen / core closed) gen=%llu\n",
                             static_cast<unsigned long long>(fs.generation));
              }
            }
            tvCoreOpen = open;
            {
                std::lock_guard<std::recursive_mutex> lock(tvMu);
                const auto started = player.startedPlayback();
                if (!misterplex::spelunkr::rememberPresentedChannel(
                        *tvSess, started.generation, lastPlayedChannel,
                        [&](const std::string& id) {
                            return upsertConfKey(confPath, "SPELUNKR_LAST_CHANNEL", id);
                        }))
                    std::fprintf(stderr, "misterplexd: failed to persist Spelunkr last presented channel\n");
            }
        }
        if (!serviceReadyWritten && player.serviceReady() &&
            (!spelunkrOn || (tvSess && (tvResumeGeneration == 0 ||
                player.startedPlayback().generation == tvResumeGeneration)))) {
            std::ofstream ready(misterplex::service::kReadyPath, std::ios::trunc);
            ready << ::getpid() << ' ' << misterplex::service::name(spelunkrOn) << '\n';
            ready.close();
            if (ready.fail())
                std::fprintf(stderr, "misterplexd: Service ready identity write failed\n");
            else
                serviceReadyWritten = true;
        }
        if (presentRetry.active() && misterplex::CoreGate::process().ownsIdleInFabric()) {
            presentRetry.standDown();   // #344: PlexOB owns the display; no legacy re-init
            std::fprintf(stderr, "misterplexd: present retry stood down after %d attempts: PlexOB core "
                                 "owns the display (OwenbRing); legacy present not needed\n",
                         presentRetry.attempts());
        }
        if (presentRetry.due(std::chrono::steady_clock::now())) {
            const bool ok = player.initPresent();
            const int64_t nextMs = presentRetry.record(ok, std::chrono::steady_clock::now());
            if (ok) {
                std::fprintf(stderr, "misterplexd: present retry %d ok -> starting idle/OSD/input workers\n",
                             presentRetry.attempts());
                startPresentWorkers();
            } else if (nextMs > 0) {
                std::fprintf(stderr, "misterplexd: present retry %d/%d failed; next in %lld ms\n",
                             presentRetry.attempts(), misterplex::PresentRetry::kMaxAttempts,
                             static_cast<long long>(nextMs));
            } else {
                std::fprintf(stderr, "misterplexd: present retry gave up after %d attempts (companion only)\n",
                             presentRetry.attempts());
            }
        }
        if (!spelunkrOn && player.owenbIdleWanted()) {   // OWENB-P1B T7: the PMS screen on the OB idle card
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
        if (!spelunkrOn && misterplex::ambientShouldFetch(misterplex::ambientEnabled(), player.owenbIdleWanted(), player.playing(),
                                            ambientBusy.load(), !defaultPms.empty() && !readConfToken().empty(),
                                            ambientNowMs(), ambientFetchedAt.load(), ambientCards.load())) {
            ambientBusy.store(true);
            if (ambientThr.joinable())
                ambientThr.join();
            const std::string ambientBase = defaultPms;
            const std::string ambientToken = readConfToken();
            ambientThr = std::thread([&, ambientBase, ambientToken] {
                int n = 0;
                try {
                    const auto deck = fetchAmbientDeck(ambientBase, ambientToken);
                    n = deck ? static_cast<int>(deck->cards.size()) : 0;
                    if (n > 0)
                        player.setAmbientDeck(deck);
                } catch (...) {
                    n = 0;
                }
                if (n > 0) {
                    ambientCards.store(n);
                    std::fprintf(stderr, "misterplexd: AMBIENT armed cards=%d dwell=%lld\n", n,
                                 static_cast<long long>(misterplex::kAmbientDwellMs));
                } else if (ambientCards.load() == 0) {
                    std::fprintf(stderr, "misterplexd: AMBIENT no art\n");
                }
                ambientFetchedAt.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count());
                ambientBusy.store(false);
            });
        }
        if (++tick % 3 != 0)
            continue;
        misterplex::FpgaSpi::resumeStrandedMain();
    }
    ytWarm.stop();
    if (ambientThr.joinable())
        ambientThr.join();
    player.recoverStrandedMenuCore(false);
    misterplex::stopWallClockRescue();
    probeRun.store(false);
    if (probeThr.joinable())
        probeThr.join();

    quiesceSpelunkr();
    {
        std::lock_guard<std::mutex> handoff(playHandoffMu);
        misterplex::retirePlayback(player, pmsTimeline);
    }
    // Daemon exit: a debounced remember-picture write (3 s) must reach disk now.
    if (!misterplex::processRememberPicture().flushPending())
        std::fprintf(stderr, "misterplexd: remember picture exit flush failed\n");
    // Clean stop on a status block row: H[6:1] = 0 (server -, Idle, fps -), H[0] kept, so the core's
    // 360-field timeout shows Not running from the last beat.
    if (hbWritten && player.statusBlockActive())
        player.writeStatusHeartbeat(hb::stopValue(hbLast));
    airplayFe.stop();
    ytFe.stop();
    if (spelunkrOn)
        player.shutdown();   // Join input readers while their callback captures are alive.
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
    // idle-card:daemon-stop -- a clean stop (INT/TERM): leave "daemon not running" on screen, not whatever is behind
    // us. The painter retries the ring every 2 s until this process has closed its session. MPX_IDLE_CARD=0 skips.
    if (const char* ic = std::getenv("MPX_IDLE_CARD"); !(ic && std::strcmp(ic, "0") == 0)) {
        const std::string exe = std::string(std::getenv("MISTERPLEX_IDLE_CARD") ? std::getenv("MISTERPLEX_IDLE_CARD")
                                                                                 : "/media/fat/misterplex/bin/owenb_idle_card");
        if (misterplex::owenb_idle_card::spawn(exe, "down"))
            std::fprintf(stderr, "misterplexd: daemon stopped; %s paints \"daemon not running\"\n", exe.c_str());
    }
    return 0;
}
