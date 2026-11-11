#pragma once
#include "airplay/mini_json.hpp"
#include "airplay/yt_cache.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace misterplex::airplay::yt_warm {

inline std::string setting(const char* key, const char* env, const char* fallback) {
    if (const char* v = std::getenv(env); v && *v)
        return v;
    if (const char* v = std::getenv(key); v && *v)
        return v;
    return fallback;
}

inline bool enabled() {
    const std::string v = setting("YT_WARM", "MPX_YT_WARM", "1");
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

struct Options {
    std::string socket =
        setting("YT_WARM_SOCK", "MPX_YT_WARM_SOCK", "/media/fat/misterplex/yt-warm.sock");
    std::string script =
        setting("YT_WARM_SCRIPT", "MPX_YT_WARM_SCRIPT", "/media/fat/misterplex/yt_warm_helper.py");
    std::string python = "python3";
    std::string cacheDir = yt_dlp::ytCacheDir();
    std::string ytdlpBin = setting("YTDLP_BIN", "MPX_YTDLP_BIN", "");
    std::vector<std::string> deployLocks = {"/tmp/misterplex-deploy.lock", "/tmp/misterplex-restart.lock",
                                            "/tmp/misterplexd-boot.lock.d"};
    unsigned rssCapKb = 96u * 1024u;
    int backoffMs = 1000;
    int maxBackoffMs = 60000;
    int startupMs = 15000; // cold A9 python3+ssl can exceed 5s
};

struct Result {
    std::string url, audioUrl, title;
};

class Supervisor {
    using Clock = std::chrono::steady_clock;

  public:
    explicit Supervisor(Options opt = {}) : opt_(std::move(opt)) {}
    ~Supervisor() { stop(); }
    Supervisor(const Supervisor&) = delete;
    Supervisor& operator=(const Supervisor&) = delete;

    void startAsync() {
        if (thread_.joinable() || !enabled())
            return;
        active_.store(true);
        thread_ = std::thread([this] { supervise(); });
    }

    void stop() {
        active_.store(false);
        ready_.store(false);
        wake_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }

    bool ready() const { return ready_.load(); }
    pid_t pid() const { return pid_.load(); }

    std::optional<Result> resolve(const std::string& videoId, int timeoutMs,
                                  const std::function<bool()>& abort = {}) {
        if (!ready() || videoId.size() != 11 ||
            videoId.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQR"
                                      "STUVWXYZ0123456789_-") != std::string::npos ||
            timeoutMs <= 0)
            return {};
        std::unique_lock<std::mutex> single(requestMu_, std::try_to_lock);
        if (!single.owns_lock())
            return {};
        const pid_t generation = pid();
        Json response;
        const int budget = std::min(timeoutMs, 5000);
        const std::string request = "{\"op\":\"resolve\",\"videoId\":\"" + videoId +
                                    "\",\"timeoutMs\":" + std::to_string(budget) + "}\n";
        const bool ok = exchange(request, response, budget, abort);
        if (!ok || !active_.load() || pid() != generation) {
            if (active_.load()) {
                ready_.store(false);
                restart_.store(true);
                wake_.notify_all();
                std::fprintf(stderr, "misterplexd: yt-warm transport failed; using one-shot\n");
            }
            return {};
        }
        const Json* success = response.get("ok");
        if (!success || success->type != Json::Bool || !success->b) {
            std::fprintf(stderr, "misterplexd: yt-warm unavailable for video; using one-shot\n");
            return {};
        }
        Result r;
        const Json* url = response.get("url");
        const Json* audio = response.get("audioUrl");
        const Json* title = response.get("title");
        if (!url || url->type != Json::Str || (audio && audio->type != Json::Str) ||
            (title && title->type != Json::Str)) {
            std::fprintf(stderr, "misterplexd: yt-warm invalid response; using one-shot\n");
            return {};
        }
        r.url = url->str;
        if (audio)
            r.audioUrl = audio->str;
        if (title)
            r.title = title->str;
        return r;
    }

  private:
    Options opt_;
    std::atomic<bool> active_{false}, ready_{false}, restart_{false};
    std::atomic<pid_t> pid_{-1};
    std::thread thread_;
    std::mutex requestMu_, waitMu_;
    std::condition_variable wake_;

    bool deploying() const {
        const char* flag = std::getenv("MPX_DEPLOYING");
        if (flag && *flag && std::strcmp(flag, "0") != 0)
            return true;
        for (const auto& path : opt_.deployLocks) {
            if (::access(path.c_str(), F_OK) == 0)
                return true;
        }
        // Candidate deploy uses flock on a persistent file, not its existence.
        const int fd = ::open("/media/fat/misterplex/run/candidate-deploy.lock", O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return false;
        const bool locked = ::flock(fd, LOCK_SH | LOCK_NB) != 0;
        ::close(fd);
        return locked;
    }

    void pause(int ms) {
        std::unique_lock<std::mutex> lk(waitMu_);
        wake_.wait_for(lk, std::chrono::milliseconds(ms),
                       [this] { return !active_.load() || restart_.load(); });
    }

    bool exchange(const std::string& request, Json& response, int timeoutMs,
                  const std::function<bool()>& abort = {}) const {
        if (opt_.socket.size() + 1 >= sizeof(sockaddr_un::sun_path))
            return false;
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return false;
        struct Close {
            int fd;
            ~Close() { ::close(fd); }
        } close{fd};
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::ifstream marker(opt_.socket);
        std::string kind;
        std::getline(marker, kind);
        const bool abstract = kind == "abstract";
        std::memcpy(address.sun_path + (abstract ? 1 : 0), opt_.socket.c_str(), opt_.socket.size() + 1);
        const socklen_t len =
            abstract ? offsetof(sockaddr_un, sun_path) + 1 + opt_.socket.size() : sizeof(address);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), len) != 0)
            return false; // A warming/busy worker never queues a cast behind startup.
        const auto until = Clock::now() + std::chrono::milliseconds(timeoutMs);
        size_t sent = 0;
        std::string reply;
        while (active_.load() && Clock::now() < until && !(abort && abort())) {
            const short events = sent < request.size() ? POLLOUT : POLLIN;
            pollfd pfd{fd, events, 0};
            const int polled = ::poll(&pfd, 1, 25);
            if (polled < 0 && errno == EINTR)
                continue;
            if (polled < 0 || (pfd.revents & (POLLERR | POLLNVAL)))
                return false;
            if (polled == 0)
                continue;
            if (sent < request.size()) {
                const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
                if (n > 0)
                    sent += static_cast<size_t>(n);
                else if (n == 0 || (errno != EAGAIN && errno != EINTR))
                    return false;
            } else {
                char buffer[4096];
                const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
                if (n > 0) {
                    reply.append(buffer, static_cast<size_t>(n));
                    if (reply.size() > 65536)
                        return false;
                    const size_t end = reply.find('\n');
                    if (end != std::string::npos)
                        return parseJson(reply.substr(0, end), response);
                } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                    return false;
                }
            }
        }
        return false;
    }

    unsigned rssKb(pid_t pid) const {
        std::ifstream in("/proc/" + std::to_string(pid) + "/status");
        std::string line;
        while (std::getline(in, line)) {
            unsigned kb = 0;
            if (std::sscanf(line.c_str(), "VmRSS: %u", &kb) == 1)
                return kb;
        }
        return 0;
    }

    // Orphans after misterplexd kill -9 keep the abstract AF_UNIX name (P2-a).
    void killStaleHelpers() const {
        DIR* proc = ::opendir("/proc");
        if (!proc)
            return;
        const pid_t self = ::getpid();
        while (dirent* entry = ::readdir(proc)) {
            char* end = nullptr;
            const long pid = std::strtol(entry->d_name, &end, 10);
            if (!*entry->d_name || !end || *end || pid <= 1 || pid == self)
                continue;
            const std::string cmdPath = "/proc/" + std::to_string(pid) + "/cmdline";
            std::ifstream in(cmdPath);
            if (!in)
                continue;
            std::string cmd((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            // Require our socket endpoint in argv so editors/tests/ssh shells
            // that merely mention yt_warm_helper.py are not SIGKILL'd (P3-1).
            const std::string sockArg = std::string("--socket") + '\0' + opt_.socket + '\0';
            const bool nameHit = cmd.find("yt_warm_helper.py") != std::string::npos ||
                                 cmd.find(opt_.script) != std::string::npos;
            if (!nameHit || cmd.find(sockArg) == std::string::npos)
                continue;
            std::fprintf(stderr, "misterplexd: yt-warm killing stale helper pid=%ld\n", pid);
            ::kill(static_cast<pid_t>(pid), SIGKILL);
            // Best-effort reap if we are subreaper; ignore failures.
            ::waitpid(static_cast<pid_t>(pid), nullptr, WNOHANG);
        }
        ::closedir(proc);
        ::unlink(opt_.socket.c_str());
    }

    void supervise() {
        int backoff = opt_.backoffMs;
        while (active_.load()) {
            restart_.store(false);
            if (deploying()) {
                pause(100);
                continue;
            }
            const int lock = ::open((opt_.socket + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
            if (lock < 0 || ::flock(lock, LOCK_EX | LOCK_NB) != 0) {
                if (lock >= 0)
                    ::close(lock);
                std::fprintf(stderr, "misterplexd: yt-warm endpoint lock unavailable; retrying\n");
                pause(backoff);
                backoff = std::min(backoff * 2, opt_.maxBackoffMs);
                continue;
            }
            killStaleHelpers();
            ::unlink(opt_.socket.c_str());
            const std::string parentPid = std::to_string(::getpid());
            std::vector<std::string> args = {opt_.python,   "-B",         "-u",          opt_.script,
                                             "--socket",    opt_.socket,  "--cache-dir", opt_.cacheDir,
                                             "--ytdlp-bin", opt_.ytdlpBin, "--parent-pid", parentPid};
            std::vector<char*> argv;
            for (auto& arg : args)
                argv.push_back(arg.data());
            argv.push_back(nullptr);
            posix_spawnattr_t attr;
            posix_spawnattr_init(&attr);
            posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
            posix_spawnattr_setpgroup(&attr, 0);
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
            posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, (opt_.socket + ".log").c_str(),
                                             O_WRONLY | O_CREAT | O_TRUNC, 0600);
            posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
            // Do not let the worker keep companion listeners, FPGA mappings or
            // other non-CLOEXEC daemon descriptors alive across shutdown.
            DIR* fds = ::opendir("/proc/self/fd");
            if (!fds) {
                std::fprintf(stderr, "misterplexd: yt-warm cannot enumerate descriptors\n");
                posix_spawn_file_actions_destroy(&actions);
                posix_spawnattr_destroy(&attr);
                ::close(lock);
                pause(backoff);
                backoff = std::min(backoff * 2, opt_.maxBackoffMs);
                continue;
            }
            while (dirent* entry = ::readdir(fds)) {
                char* end = nullptr;
                const long fd = std::strtol(entry->d_name, &end, 10);
                if (*entry->d_name && end && !*end && fd > STDERR_FILENO)
                    posix_spawn_file_actions_addclose(&actions, static_cast<int>(fd));
            }
            ::closedir(fds);
            pid_t child = -1;
            int error = ::posix_spawnp(&child, argv[0], &actions, &attr, argv.data(), environ);
            if (error == ENOENT && opt_.python == "python3") {
                args[0] = "python";
                argv[0] = args[0].data();
                error = ::posix_spawnp(&child, argv[0], &actions, &attr, argv.data(), environ);
            }
            posix_spawn_file_actions_destroy(&actions);
            posix_spawnattr_destroy(&attr);
            const auto born = Clock::now();
            if (error == 0) {
                pid_.store(child);
                if (::setpriority(PRIO_PROCESS, child, 10) != 0)
                    std::fprintf(stderr, "misterplexd: yt-warm nice failed: %s\n", std::strerror(errno));
                bool reaped = false;
                while (active_.load() && !restart_.load() && !deploying()) {
                    int status = 0;
                    const pid_t waited = ::waitpid(child, &status, WNOHANG);
                    if (waited == child || (waited < 0 && errno == ECHILD)) {
                        reaped = true;
                        std::fprintf(stderr, "misterplexd: yt-warm exited; retrying with backoff\n");
                        break;
                    }
                    if (rssKb(child) > opt_.rssCapKb) {
                        std::fprintf(stderr, "misterplexd: yt-warm RSS cap exceeded; killing group\n");
                        break;
                    }
                    if (!ready_.load()) {
                        Json pong;
                        if (exchange("{\"op\":\"ping\"}\n", pong, 25)) {
                            const Json* ok = pong.get("ok");
                            ready_.store(ok && ok->type == Json::Bool && ok->b);
                        }
                        if (!ready_.load() &&
                            Clock::now() - born > std::chrono::milliseconds(opt_.startupMs)) {
                            std::fprintf(stderr, "misterplexd: yt-warm startup timed out\n");
                            break;
                        }
                    }
                    pause(100);
                }
                ready_.store(false);
                ::kill(-child, SIGKILL); // Includes descendants, even when the leader
                                         // already exited.
                if (!reaped) {
                    while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
                    }
                }
                pid_.store(-1);
            } else {
                std::fprintf(stderr, "misterplexd: yt-warm spawn failed: %s\n", std::strerror(error));
            }
            ::unlink(opt_.socket.c_str());
            ::close(lock);
            if (Clock::now() - born > std::chrono::seconds(60))
                backoff = opt_.backoffMs;
            restart_.store(false);
            pause(backoff);
            backoff = std::min(backoff * 2, opt_.maxBackoffMs);
        }
    }
};

inline Supervisor& process() {
    static Supervisor supervisor;
    return supervisor;
}

} // namespace misterplex::airplay::yt_warm
