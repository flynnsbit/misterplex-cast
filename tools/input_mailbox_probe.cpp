// Read/watch the MiSTerPlex DDR playback-input mailbox from /dev/mem.
// This tool is validation-only: it never touches the HPS<->FPGA SPI register.
// The PLX page comes from the build's fpga_build_ids.hpp row (--build); an
// unknown build gets no PLX reads, and a page whose PLXS heartbeat does not move
// is refused instead of being read as "no command".

#include "libmisterplex/fpga_build_ids.hpp"
#include "libmisterplex/input_mailbox.hpp"
#include "libmisterplex/plx_page.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr size_t kMapLen = 0x1000u;
constexpr size_t kMailboxOff = misterplex::kInputMailboxOff;

const char* cmdName(misterplex::PlaybackCommand cmd) {
    switch (cmd) {
    case misterplex::PlaybackCommand::None:
        return "none";
    case misterplex::PlaybackCommand::PlayPause:
        return "playpause";
    case misterplex::PlaybackCommand::Stop:
        return "stop";
    case misterplex::PlaybackCommand::SkipForward:
        return "skipforward";
    case misterplex::PlaybackCommand::SkipBack:
        return "skipback";
    case misterplex::PlaybackCommand::Browse:
        return "browse";
    }
    return "unknown";
}

bool parseCmd(const std::string& s, misterplex::PlaybackCommand& out, bool& any) {
    any = false;
    if (s == "any") {
        any = true;
        out = misterplex::PlaybackCommand::None;
        return true;
    }
    if (s == "none") {
        out = misterplex::PlaybackCommand::None;
        return true;
    }
    if (s == "playpause" || s == "play-pause" || s == "play_pause") {
        out = misterplex::PlaybackCommand::PlayPause;
        return true;
    }
    if (s == "stop") {
        out = misterplex::PlaybackCommand::Stop;
        return true;
    }
    if (s == "skipforward" || s == "skip-forward" || s == "right") {
        out = misterplex::PlaybackCommand::SkipForward;
        return true;
    }
    if (s == "skipback" || s == "skip-back" || s == "left") {
        out = misterplex::PlaybackCommand::SkipBack;
        return true;
    }
    return false;
}

struct Reader {
    int fd = -1;
    uint8_t* map = nullptr;
    uint32_t page = 0;

    ~Reader() {
        if (map)
            munmap(map, kMapLen);
        if (fd >= 0)
            close(fd);
    }

    bool openMem(const char* memPath, uint32_t pagePhys) {
        page = pagePhys;
        fd = ::open(memPath, O_RDONLY | O_SYNC | O_CLOEXEC);
        if (fd < 0) {
            std::fprintf(stderr, "open %s: %s\n", memPath, std::strerror(errno));
            return false;
        }
        void* p = mmap(nullptr, kMapLen, PROT_READ, MAP_SHARED, fd, static_cast<off_t>(page));
        if (p == MAP_FAILED) {
            std::fprintf(stderr, "mmap 0x%08x: %s\n", page, std::strerror(errno));
            return false;
        }
        map = static_cast<uint8_t*>(p);
        return true;
    }

    // The core republishes PLXS (seq in the high word) at least every 2^18
    // clk_ddr, so on the live core's page the high word moves within 50 ms.
    bool heartbeatLive() const {
        volatile uint32_t* s = reinterpret_cast<volatile uint32_t*>(map + mailbox_abi::kPlxsOff);
        const uint32_t lo0 = s[0];
        const uint32_t hi0 = s[1];
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const uint32_t lo1 = s[0];
        const uint32_t hi1 = s[1];
        return lo0 == mailbox_abi::kPlxsMagic && lo1 == mailbox_abi::kPlxsMagic && hi0 != hi1;
    }

    enum class Status { Ok, NoMagic, Torn, Invalid };

    Status read(misterplex::InputMailboxSample& out, uint64_t& raw) const {
        volatile uint32_t* mb = reinterpret_cast<volatile uint32_t*>(map + kMailboxOff);
        for (int attempt = 0; attempt < 4; ++attempt) {
            const uint32_t lo = mb[0];
            const uint32_t hi = mb[1];
            __sync_synchronize();
            const uint32_t lo2 = mb[0];
            const uint32_t hi2 = mb[1];
            if (lo != lo2 || hi != hi2)
                continue;
            raw = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
            if (lo != misterplex::kInputMailboxMagic)
                return Status::NoMagic;
            return misterplex::decodeInputMailboxWord(raw, out) ? Status::Ok : Status::Invalid;
        }
        return Status::Torn;
    }
};

void printSample(const char* prefix, const misterplex::InputMailboxSample& s, uint64_t raw) {
    std::printf("%s raw=0x%016llx magic=PLXI seq=%u cmd_seq=%u cmd=%s(%u)\n", prefix,
                static_cast<unsigned long long>(raw), static_cast<unsigned>(s.seq),
                static_cast<unsigned>(s.cmdSeq), cmdName(s.command),
                static_cast<unsigned>(static_cast<uint8_t>(s.command)));
}

int usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s --build 0xID --once\n"
                 "       %s --build 0xID --expect any|playpause|stop|skipforward|skipback "
                 "[--timeout-ms N] [--settle-ms N]\n"
                 "  --build: FPGA_VIDEO_BUILD_ID of the loaded RBF; the PLX page is its\n"
                 "           fpga_build_ids.hpp row (unknown build: no PLX reads)\n"
                 "  --mem PATH: memory image instead of /dev/mem (offline tests)\n",
                 argv0, argv0);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    bool once = false;
    bool wait = false;
    bool any = true;
    int timeoutMs = 10000;
    int settleMs = 700;
    bool haveBuild = false;
    uint32_t buildId = 0;
    const char* memPath = "/dev/mem";
    misterplex::PlaybackCommand expect = misterplex::PlaybackCommand::None;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--once") {
            once = true;
        } else if (arg == "--expect" && i + 1 < argc) {
            wait = true;
            if (!parseCmd(argv[++i], expect, any))
                return usage(argv[0]);
        } else if (arg == "--timeout-ms" && i + 1 < argc) {
            timeoutMs = std::atoi(argv[++i]);
        } else if (arg == "--settle-ms" && i + 1 < argc) {
            settleMs = std::atoi(argv[++i]);
        } else if (arg == "--build" && i + 1 < argc) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(argv[++i], &end, 0);
            if (!end || *end || v > 0xFFFFFFFFul)
                return usage(argv[0]);
            buildId = static_cast<uint32_t>(v);
            haveBuild = true;
        } else if (arg == "--mem" && i + 1 < argc) {
            memPath = argv[++i];
        } else {
            return usage(argv[0]);
        }
    }
    if (once == wait || !haveBuild)
        return usage(argv[0]);

    const misterplex::FpgaBuildInfo* row = misterplex::recognisedFpgaBuild(buildId);
    const uint32_t page = misterplex::fpgaPlxPage(row);
    if (!row || page == 0) {
        std::printf("FAIL build 0x%08X has no fpga_build_ids.hpp row: no PLX page, no PLX reads\n",
                    buildId);
        return 6;
    }
    std::printf("page=0x%08X build=0x%08X class=%s\n", page, buildId,
                misterplex::plxPageClassName(row->plxPage));

    Reader r;
    if (!r.openMem(memPath, page))
        return 1;
    if (!r.heartbeatLive()) {
        std::printf("FAIL page 0x%08X: PLXS heartbeat (+0x%03X) not moving; the loaded core is "
                    "not build 0x%08X or nothing publishes there\n",
                    page, mailbox_abi::kPlxsOff + 4u, buildId);
        return 5;
    }

    if (once) {
        misterplex::InputMailboxSample s;
        uint64_t raw = 0;
        const auto st = r.read(s, raw);
        if (st == Reader::Status::Ok) {
            printSample("mailbox", s, raw);
            return 0;
        }
        if (st == Reader::Status::NoMagic) {
            std::printf("mailbox raw=0x%016llx magic=BAD\n",
                        static_cast<unsigned long long>(raw));
            return 3;
        }
        std::printf("mailbox %s\n", st == Reader::Status::Torn ? "torn" : "invalid");
        return 4;
    }

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    bool haveBase = false;
    bool sawNoMagic = false;
    uint8_t baseCmdSeq = 0;
    uint16_t baseSeq = 0;
    bool haveFirst = false;
    int events = 0;
    misterplex::InputMailboxSample first;
    uint64_t firstRaw = 0;
    auto settleDeadline = deadline;

    while (Clock::now() < deadline) {
        misterplex::InputMailboxSample s;
        uint64_t raw = 0;
        const auto st = r.read(s, raw);
        if (st == Reader::Status::NoMagic && !haveBase)
            sawNoMagic = true;
        if (st != Reader::Status::Ok) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (!haveBase && !sawNoMagic) {
            haveBase = true;
            baseCmdSeq = s.cmdSeq;
            baseSeq = s.seq;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (!haveBase || s.cmdSeq != baseCmdSeq) {
            ++events;
            haveBase = true;
            baseCmdSeq = s.cmdSeq;
            baseSeq = s.seq;
            if (!haveFirst) {
                haveFirst = true;
                first = s;
                firstRaw = raw;
                settleDeadline = Clock::now() + std::chrono::milliseconds(settleMs);
            }
        } else if (s.seq != baseSeq) {
            // seq moved without cmd_seq: report it, but do not count as a command.
            baseSeq = s.seq;
        }
        if (haveFirst && Clock::now() >= settleDeadline)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!haveFirst) {
        std::printf("FAIL timeout waiting for mailbox command\n");
        return 10;
    }
    printSample(events == 1 ? "event" : "event_first", first, firstRaw);
    std::printf("events=%d settle_ms=%d\n", events, settleMs);
    if (events != 1) {
        std::printf("FAIL expected exactly one cmd_seq advance\n");
        return 11;
    }
    if (!any && first.command != expect) {
        std::printf("FAIL expected cmd=%s got=%s\n", cmdName(expect), cmdName(first.command));
        return 12;
    }
    std::printf("PASS input mailbox command exactly once\n");
    return 0;
}
