#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

int64_t wallMicros() {
    const auto now = Clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

int64_t threadCpuMicros() {
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return 0;
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + static_cast<int64_t>(ts.tv_nsec) / 1000;
}

double timevalSec(const timeval& tv) {
    return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1e6;
}

struct ReadStats {
    int64_t bytes = 0;
    int64_t calls = 0;
    int64_t ok_calls = 0;
    int64_t eagain = 0;
    int64_t eintr = 0;
    int64_t zero = 0;
    int64_t read_wall_us = 0;
    int64_t read_cpu_us = 0;
    int64_t syscall_wall_us = 0;
    int64_t syscall_cpu_us = 0;
    int64_t sleep_wall_us = 0;
    int64_t sleep_cpu_us = 0;
    int64_t copy_wall_us = 0;
    int64_t copy_cpu_us = 0;
    int64_t max_read = 0;
    uint64_t checksum = 0;
};

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage: %s --label NAME [--frame-bytes N] [--copy] [--pipe-size N] -- CMD...\n"
                 "Runs CMD, measures child CPU via wait4, and optionally drains CMD stdout as "
                 "fixed-size rawvideo frames.\n",
                 argv0);
}

std::vector<char*> makeArgv(std::vector<std::string>& args) {
    std::vector<char*> out;
    out.reserve(args.size() + 1);
    for (auto& a : args)
        out.push_back(a.data());
    out.push_back(nullptr);
    return out;
}

ReadStats drainPipe(int fd, size_t frameBytes, bool doCopy) {
    ReadStats st;
    std::vector<uint8_t> buf(std::max<size_t>(4096, std::min<size_t>(frameBytes, 1u << 20)));
    std::vector<uint8_t> frame;
    if (doCopy)
        frame.resize(frameBytes);
    size_t frameOff = 0;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    const int64_t readWall0 = wallMicros();
    const int64_t readCpu0 = threadCpuMicros();
    for (;;) {
        ++st.calls;
        const int64_t sw0 = wallMicros();
        const int64_t sc0 = threadCpuMicros();
        const ssize_t n = ::read(fd, buf.data(), buf.size());
        st.syscall_wall_us += wallMicros() - sw0;
        st.syscall_cpu_us += threadCpuMicros() - sc0;

        if (n < 0) {
            if (errno == EINTR) {
                ++st.eintr;
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ++st.eagain;
                const int64_t sleepWall0 = wallMicros();
                const int64_t sleepCpu0 = threadCpuMicros();
                usleep(2000);
                st.sleep_wall_us += wallMicros() - sleepWall0;
                st.sleep_cpu_us += threadCpuMicros() - sleepCpu0;
                continue;
            }
            std::perror("read");
            break;
        }
        if (n == 0) {
            ++st.zero;
            break;
        }
        ++st.ok_calls;
        st.bytes += n;
        st.max_read = std::max<int64_t>(st.max_read, n);

        if (doCopy && frameBytes > 0) {
            size_t off = 0;
            while (off < static_cast<size_t>(n)) {
                const size_t take =
                    std::min(frameBytes - frameOff, static_cast<size_t>(n) - off);
                const int64_t cw0 = wallMicros();
                const int64_t cc0 = threadCpuMicros();
                std::memcpy(frame.data() + frameOff, buf.data() + off, take);
                st.copy_wall_us += wallMicros() - cw0;
                st.copy_cpu_us += threadCpuMicros() - cc0;
                frameOff += take;
                off += take;
                if (frameOff == frameBytes) {
                    st.checksum += frame[(st.bytes / std::max<size_t>(1, frameBytes)) % frameBytes];
                    frameOff = 0;
                }
            }
        }
    }
    st.read_wall_us = wallMicros() - readWall0;
    st.read_cpu_us = threadCpuMicros() - readCpu0;
    return st;
}

} // namespace

int main(int argc, char** argv) {
    std::string label = "probe";
    size_t frameBytes = 0;
    int pipeSize = 0;
    bool doCopy = false;
    int cmdStart = -1;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", opt);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--label") {
            label = need("--label");
        } else if (a == "--frame-bytes") {
            frameBytes = static_cast<size_t>(std::strtoull(need("--frame-bytes"), nullptr, 0));
        } else if (a == "--pipe-size") {
            pipeSize = std::atoi(need("--pipe-size"));
        } else if (a == "--copy") {
            doCopy = true;
        } else if (a == "--") {
            cmdStart = i + 1;
            break;
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }
    if (cmdStart < 0 || cmdStart >= argc) {
        usage(argv[0]);
        return 2;
    }

    std::vector<std::string> cmd;
    for (int i = cmdStart; i < argc; ++i)
        cmd.emplace_back(argv[i]);

    int p[2] = {-1, -1};
    if (frameBytes > 0 && pipe(p) != 0) {
        std::perror("pipe");
        return 1;
    }
#ifdef F_SETPIPE_SZ
    int actualPipeSize = -1;
    if (frameBytes > 0) {
        if (pipeSize > 0)
            (void)fcntl(p[0], F_SETPIPE_SZ, pipeSize);
        actualPipeSize = fcntl(p[0], F_GETPIPE_SZ, 0);
    }
#else
    const int actualPipeSize = -1;
#endif

    const int64_t wall0 = wallMicros();
    pid_t pid = fork();
    if (pid < 0) {
        std::perror("fork");
        return 1;
    }
    if (pid == 0) {
        if (frameBytes > 0) {
            close(p[0]);
            dup2(p[1], STDOUT_FILENO);
            close(p[1]);
        }
        auto av = makeArgv(cmd);
        execvp(av[0], av.data());
        std::perror("execvp");
        _exit(127);
    }

    ReadStats readStats;
    if (frameBytes > 0) {
        close(p[1]);
        readStats = drainPipe(p[0], frameBytes, doCopy);
        close(p[0]);
    }

    int status = 0;
    rusage ru{};
    if (wait4(pid, &status, 0, &ru) < 0) {
        std::perror("wait4");
        return 1;
    }
    const int64_t wallUs = wallMicros() - wall0;
    const double wallSec = static_cast<double>(std::max<int64_t>(1, wallUs)) / 1e6;
    const double childUser = timevalSec(ru.ru_utime);
    const double childSys = timevalSec(ru.ru_stime);
    const double childCpu = childUser + childSys;
    const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    const int64_t frames = frameBytes > 0 ? readStats.bytes / static_cast<int64_t>(frameBytes) : 0;
    const double fps = frames > 0 ? static_cast<double>(frames) / wallSec : 0.0;

    std::printf(
        "ffmpeg_probe label=%s exit=%d wall_s=%.6f child_user_s=%.6f child_sys_s=%.6f "
        "child_cpu_s=%.6f child_cpu_pct_1c=%.1f child_cpu_pct_2c=%.1f maxrss_kb=%ld "
        "nvcsw=%ld nivcsw=%ld frame_bytes=%zu frames=%lld fps=%.3f bytes=%lld "
        "pipe_size=%d read_calls=%lld read_ok_calls=%lld read_eagain=%lld read_zero=%lld "
        "read_wall_s=%.6f read_cpu_s=%.6f syscall_cpu_s=%.6f sleep_wall_s=%.6f "
        "sleep_cpu_s=%.6f copy_wall_s=%.6f copy_cpu_s=%.6f max_read=%lld checksum=%llu\n",
        label.c_str(), exitCode, wallSec, childUser, childSys, childCpu,
        100.0 * childCpu / wallSec, 50.0 * childCpu / wallSec, ru.ru_maxrss, ru.ru_nvcsw,
        ru.ru_nivcsw, frameBytes, static_cast<long long>(frames), fps,
        static_cast<long long>(readStats.bytes), actualPipeSize,
        static_cast<long long>(readStats.calls), static_cast<long long>(readStats.ok_calls),
        static_cast<long long>(readStats.eagain), static_cast<long long>(readStats.zero),
        static_cast<double>(readStats.read_wall_us) / 1e6,
        static_cast<double>(readStats.read_cpu_us) / 1e6,
        static_cast<double>(readStats.syscall_cpu_us) / 1e6,
        static_cast<double>(readStats.sleep_wall_us) / 1e6,
        static_cast<double>(readStats.sleep_cpu_us) / 1e6,
        static_cast<double>(readStats.copy_wall_us) / 1e6,
        static_cast<double>(readStats.copy_cpu_us) / 1e6,
        static_cast<long long>(readStats.max_read),
        static_cast<unsigned long long>(readStats.checksum));
    return exitCode == 0 ? 0 : exitCode;
}
