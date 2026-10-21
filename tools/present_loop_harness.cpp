#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Sample {
    int64_t wall_us = 0;
    int64_t cpu_us = 0;
};

struct ReadStats {
    int frames = 0;
    int64_t calls = 0;
    int64_t ok_calls = 0;
    int64_t eagain = 0;
    int64_t eintr = 0;
    int64_t zero = 0;
    int64_t bytes = 0;
    int64_t max_bytes = 0;
    int64_t wall_us = 0;
    int64_t cpu_us = 0;
    int64_t syscall_wall_us = 0;
    int64_t syscall_cpu_us = 0;
    int64_t sleep_wall_us = 0;
    int64_t sleep_cpu_us = 0;
};

struct CopyStats {
    int frames = 0;
    int64_t bytes = 0;
    int64_t wall_us = 0;
    int64_t cpu_us = 0;
    uint64_t checksum = 0;
};

struct RunStats {
    ReadStats read;
    CopyStats copy;
};

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

Sample beginSample() { return Sample{wallMicros(), threadCpuMicros()}; }

Sample endSample(Sample start) {
    return Sample{wallMicros() - start.wall_us, threadCpuMicros() - start.cpu_us};
}

void fillFrame(std::vector<uint8_t>& frame) {
    uint32_t x = 0x12345678u;
    for (auto& b : frame) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b = static_cast<uint8_t>(x);
    }
}

bool writeAll(int fd, const uint8_t* p, size_t n) {
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EPIPE)
                return false;
            std::perror("write");
            return false;
        }
        p += static_cast<size_t>(w);
        n -= static_cast<size_t>(w);
    }
    return true;
}

void producer(int fd, int frames, size_t frameBytes, bool limited, double fps, size_t chunkBytes,
              std::atomic<bool>& stop) {
    std::vector<uint8_t> frame(frameBytes);
    fillFrame(frame);
    const auto t0 = Clock::now();
    const auto period = std::chrono::duration<double>(1.0 / fps);

    for (int f = 0; f < frames && !stop.load(); ++f) {
        size_t off = 0;
        if (limited)
            std::this_thread::sleep_until(t0 + std::chrono::duration_cast<Clock::duration>(
                                                   period * static_cast<double>(f)));
        while (off < frameBytes && !stop.load()) {
            const size_t n = std::min(chunkBytes, frameBytes - off);
            if (!writeAll(fd, frame.data() + off, n))
                break;
            off += n;
            if (limited && off < frameBytes) {
                const double fraction = static_cast<double>(off) / static_cast<double>(frameBytes);
                std::this_thread::sleep_until(
                    t0 + std::chrono::duration_cast<Clock::duration>(
                             period * (static_cast<double>(f) + fraction)));
            }
        }
    }
    ::close(fd);
}

RunStats consumeFrames(int fd, int frames, size_t frameBytes, bool doCopy) {
    RunStats out;
    std::vector<uint8_t> frame(frameBytes);
    std::vector<uint8_t> copy(frameBytes);

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    for (int f = 0; f < frames; ++f) {
        size_t got = 0;
        auto read0 = beginSample();
        while (got < frameBytes) {
            ++out.read.calls;
            auto syscall0 = beginSample();
            ssize_t n = ::read(fd, frame.data() + got, frameBytes - got);
            auto syscall = endSample(syscall0);
            out.read.syscall_wall_us += syscall.wall_us;
            out.read.syscall_cpu_us += syscall.cpu_us;

            if (n < 0) {
                if (errno == EINTR) {
                    ++out.read.eintr;
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    ++out.read.eagain;
                    auto sleep0 = beginSample();
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    auto sleep = endSample(sleep0);
                    out.read.sleep_wall_us += sleep.wall_us;
                    out.read.sleep_cpu_us += sleep.cpu_us;
                    continue;
                }
                std::perror("read");
                break;
            }
            if (n == 0) {
                ++out.read.zero;
                break;
            }
            ++out.read.ok_calls;
            got += static_cast<size_t>(n);
            out.read.bytes += n;
            out.read.max_bytes = std::max<int64_t>(out.read.max_bytes, n);
        }
        auto read = endSample(read0);
        out.read.wall_us += read.wall_us;
        out.read.cpu_us += read.cpu_us;
        if (got < frameBytes)
            break;
        ++out.read.frames;

        if (doCopy) {
            auto copy0 = beginSample();
            std::memcpy(copy.data(), frame.data(), frameBytes);
            auto copied = endSample(copy0);
            out.copy.wall_us += copied.wall_us;
            out.copy.cpu_us += copied.cpu_us;
            out.copy.bytes += static_cast<int64_t>(frameBytes);
            out.copy.checksum += copy[(static_cast<size_t>(f) * 131) % frameBytes];
            ++out.copy.frames;
        }
    }

    return out;
}

RunStats runPipeCase(const char* label, int frames, int w, int h, double fps, bool limited,
                     size_t chunkBytes, int pipeSize) {
    const size_t frameBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 2;
    int p[2] = {-1, -1};
    if (pipe(p) != 0) {
        std::perror("pipe");
        std::exit(1);
    }

#ifdef F_SETPIPE_SZ
    if (pipeSize > 0)
        (void)fcntl(p[0], F_SETPIPE_SZ, pipeSize);
    const int actualPipeSize = fcntl(p[0], F_GETPIPE_SZ, 0);
#else
    const int actualPipeSize = -1;
#endif

    std::atomic<bool> stop{false};
    std::thread prod(producer, p[1], frames, frameBytes, limited, fps, chunkBytes,
                     std::ref(stop));
    RunStats stats = consumeFrames(p[0], frames, frameBytes, true);
    stop.store(true);
    ::close(p[0]);
    prod.join();

    const auto& r = stats.read;
    const int readFrames = std::max(1, r.frames);
    const int64_t okCalls = std::max<int64_t>(1, r.ok_calls);
    const int64_t readLoopUs =
        std::max<int64_t>(0, r.wall_us - r.syscall_wall_us - r.sleep_wall_us);
    std::printf("pipe_case=%s frames=%d frame_bytes=%zu fps=%.3f mode=%s chunk=%zu "
                "pipe_size_req=%d pipe_size_actual=%d "
                "read_wall_us_f=%lld read_cpu_us_f=%lld read_syscall_wall_us_f=%lld "
                "read_syscall_cpu_us_f=%lld read_sleep_wall_us_f=%lld "
                "read_sleep_cpu_us_f=%lld read_loop_overhead_us_f=%lld "
                "read_calls_x100_f=%lld read_ok_calls_x100_f=%lld "
                "read_eagain_x100_f=%lld read_bytes_f=%lld read_avg_bytes_call=%lld "
                "read_max_bytes_call=%lld copy_wall_us_f=%lld copy_cpu_us_f=%lld "
                "copy_mib_s=%.1f checksum=%llu\n",
                label, r.frames, frameBytes, fps, limited ? "limited" : "unthrottled",
                chunkBytes, pipeSize, actualPipeSize,
                static_cast<long long>(r.wall_us / readFrames),
                static_cast<long long>(r.cpu_us / readFrames),
                static_cast<long long>(r.syscall_wall_us / readFrames),
                static_cast<long long>(r.syscall_cpu_us / readFrames),
                static_cast<long long>(r.sleep_wall_us / readFrames),
                static_cast<long long>(r.sleep_cpu_us / readFrames),
                static_cast<long long>(readLoopUs / readFrames),
                static_cast<long long>((r.calls * 100) / readFrames),
                static_cast<long long>((r.ok_calls * 100) / readFrames),
                static_cast<long long>((r.eagain * 100) / readFrames),
                static_cast<long long>(r.bytes / readFrames),
                static_cast<long long>(r.bytes / okCalls),
                static_cast<long long>(r.max_bytes),
                static_cast<long long>(stats.copy.wall_us / std::max(1, stats.copy.frames)),
                static_cast<long long>(stats.copy.cpu_us / std::max(1, stats.copy.frames)),
                (static_cast<double>(stats.copy.bytes) / (1024.0 * 1024.0)) /
                    (static_cast<double>(std::max<int64_t>(1, stats.copy.wall_us)) / 1e6),
                static_cast<unsigned long long>(stats.copy.checksum));
    return stats;
}

void runCopyScale() {
    const struct {
        int w;
        int h;
    } sizes[] = {{320, 240}, {640, 480}, {800, 600}};

    for (const auto& s : sizes) {
        const size_t bytes = static_cast<size_t>(s.w) * static_cast<size_t>(s.h) * 2;
        const int loops = std::max<int>(200, static_cast<int>((256ull * 1024ull * 1024ull) / bytes));
        std::vector<uint8_t> src(bytes), dst(bytes);
        fillFrame(src);
        uint64_t checksum = 0;
        auto t0 = beginSample();
        for (int i = 0; i < loops; ++i) {
            std::memcpy(dst.data(), src.data(), bytes);
            checksum += dst[(static_cast<size_t>(i) * 257) % bytes];
        }
        auto t = endSample(t0);
        const double mib = static_cast<double>(bytes) * loops / (1024.0 * 1024.0);
        const double sec = static_cast<double>(std::max<int64_t>(1, t.wall_us)) / 1e6;
        std::printf("copy_scale=%dx%d frame_bytes=%zu loops=%d wall_us_f=%lld cpu_us_f=%lld "
                    "MiB=%.1f MiBps=%.1f frames_per_s=%.1f checksum=%llu\n",
                    s.w, s.h, bytes, loops,
                    static_cast<long long>(t.wall_us / loops),
                    static_cast<long long>(t.cpu_us / loops), mib, mib / sec,
                    static_cast<double>(loops) / sec,
                    static_cast<unsigned long long>(checksum));
    }
}

} // namespace

int main(int argc, char** argv) {
    int frames = 600;
    int w = 320;
    int h = 240;
    double fps = 25.0;
    size_t chunk = 32768;
    int pipeSize = 0;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* opt) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", opt);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--frames")
            frames = std::atoi(need("--frames"));
        else if (a == "--width")
            w = std::atoi(need("--width"));
        else if (a == "--height")
            h = std::atoi(need("--height"));
        else if (a == "--fps")
            fps = std::atof(need("--fps"));
        else if (a == "--chunk")
            chunk = static_cast<size_t>(std::strtoull(need("--chunk"), nullptr, 0));
        else if (a == "--pipe-size")
            pipeSize = std::atoi(need("--pipe-size"));
        else if (a == "-h" || a == "--help") {
            std::puts("Usage: present_loop_harness [--frames N] [--width W] [--height H] "
                      "[--fps N] [--chunk BYTES] [--pipe-size BYTES]");
            return 0;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 2;
        }
    }

    if (frames <= 0 || w <= 0 || h <= 0 || fps <= 0.0 || chunk == 0) {
        std::fprintf(stderr, "bad frames/size/fps/chunk\n");
        return 2;
    }

    std::printf("present_loop_harness frame=%dx%d rgb565 bytes=%zu frames=%d fps=%.3f\n", w, h,
                static_cast<size_t>(w) * static_cast<size_t>(h) * 2, frames, fps);
    runPipeCase("unthrottled", frames, w, h, fps, false, chunk, pipeSize);
    runPipeCase("rate_limited", frames, w, h, fps, true, chunk, pipeSize);
    runCopyScale();
    return 0;
}
