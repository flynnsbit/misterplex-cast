// The fake Plex core for the core-gate daemon e2e (fake_plex_core.hpp) on the e2e's fake
// /dev/mem: the SPI handshake and the AU-ring control plane (reset ACK, CAPS with a build
// ID, session ACKs), and the MrAudio session lane (mailbox + consumer, MAST). BUILDFILE holds
// the loaded core's build ID in hex; 0 is a core that never answers CAPS (the menu, or a core
// still being configured). A changed ID is a new core instance. COUNTFILE gets "<spi strobes>
// <probes answered> <frames presented> <consumer active> <audio dispatched> <mailbox refused>
// <consumer rejected> <consumer epoch hex>" every 20 ms and at SIGTERM. AUDIOFILE (optional)
// injects into the MrAudio lane of the running instance; each new first-word sequence number
// applies its line once:
//   SEQ ignore N | SEQ reject N | SEQ wedge | SEQ unwedge | SEQ wedgeatprobe | SEQ creset |
//   SEQ silent (the whole CTRL/CAPS lane of this instance; the next load clears it) |
//   SEQ orphan EPOCH_HEX NONCE_HEX LAST_TOKEN PLAYING(0|1)
// The FPGA-manager stat and GPO core-reset bit belong to the script (Main).
// FAKE_CORE_PLX=1 (env): the core also keeps its 480p PLX page (fake_plex_core.hpp plxStep,
// kPlex480pPlxPage), as the 480p RTL does. Off by default: those words sit in the DDR window
// that core_gate_fakemem.py hash covers.
// CAPSFILE (optional, in the AUDIOFILE slot when it contains CAPS keys): lines
//   features=0x... | max_width=N | max_height=N | au_limit=N
// Any subset overrides that field; omitted fields keep fake_plex_core.hpp defaults.
//   core_gate_responder FAKEMEM COUNTFILE BUILDFILE [AUDIOFILE|CAPSFILE]
#include "fake_plex_core.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/mman.h>

#include <fcntl.h>
#include <unistd.h>

namespace {
std::atomic<bool> g_run{true};
void onTerm(int) { g_run.store(false); }

void publish(const std::string& path, const fake_core::FakePlexCore& core) {
    const std::string tmp = path + ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "w")) {
        std::fprintf(f, "%llu %llu %llu %d %llu %llu %llu %016llx\n",
                     static_cast<unsigned long long>(core.strobes()),
                     static_cast<unsigned long long>(core.probesAnswered()),
                     static_cast<unsigned long long>(core.presentedTotal()),
                     core.audioConsumerActive() ? 1 : 0,
                     static_cast<unsigned long long>(core.audioDispatched()),
                     static_cast<unsigned long long>(core.audioMailboxRefused()),
                     static_cast<unsigned long long>(core.audioConsumerRejected()),
                     static_cast<unsigned long long>(core.audioConsumerEpoch()));
        std::fclose(f);
        std::rename(tmp.c_str(), path.c_str());
    }
    // Side file for the n2 e2e retry lines; the COUNT line above keeps its format.
    const std::string audioPath = path + ".audio";
    const std::string audioTmp = audioPath + ".tmp";
    if (FILE* f = std::fopen(audioTmp.c_str(), "w")) {
        std::fprintf(f, "consumed_last_active=%llu publications=%llu cap=%d gap_pairs=%llu\n",
                     static_cast<unsigned long long>(core.audioConsumedLastActive()),
                     static_cast<unsigned long long>(core.audioPublications()),
                     core.audioConsumerCapped() ? 1 : 0,
                     static_cast<unsigned long long>(core.audioGapPairs()));
        std::fclose(f);
        std::rename(audioTmp.c_str(), audioPath.c_str());
    }
}

uint32_t readBuild(const char* path, uint32_t fallback) {
    FILE* f = std::fopen(path, "r");
    if (!f)
        return fallback;
    char buf[32]{};
    const bool ok = std::fgets(buf, sizeof buf, f) != nullptr;
    std::fclose(f);
    return ok ? static_cast<uint32_t>(std::strtoul(buf, nullptr, 16)) : fallback;
}

char* trim(char* s) {
    while (*s && std::isspace(static_cast<unsigned char>(*s)))
        ++s;
    char* e = s + std::strlen(s);
    while (e > s && std::isspace(static_cast<unsigned char>(e[-1])))
        *--e = 0;
    return s;
}

bool capsKey(const char* s) {
    return std::strncmp(s, "features=", 9) == 0 ||
           std::strncmp(s, "max_width=", 10) == 0 ||
           std::strncmp(s, "max_height=", 11) == 0 ||
           std::strncmp(s, "au_limit=", 9) == 0;
}

bool looksLikeCapsFile(const char* path) {
    if (!path)
        return false;
    FILE* f = std::fopen(path, "r");
    if (!f)
        return false;
    char line[256]{};
    bool yes = false;
    while (std::fgets(line, sizeof line, f)) {
        char* s = trim(line);
        if (*s == 0 || *s == '#')
            continue;
        if (capsKey(s)) {
            yes = true;
            break;
        }
    }
    std::fclose(f);
    return yes;
}

bool parseU32(const char* value, uint32_t& out) {
    errno = 0;
    char* end = nullptr;
    const unsigned long v = std::strtoul(value, &end, 0);
    while (end && *end && std::isspace(static_cast<unsigned char>(*end)))
        ++end;
    if (errno != 0 || !end || *end != 0 || v > 0xFFFFFFFFul)
        return false;
    out = static_cast<uint32_t>(v);
    return true;
}

bool loadCaps(const char* path, fake_core::FakePlexCore::CapsConfig& caps) {
    caps = fake_core::FakePlexCore::CapsConfig{};
    if (!path)
        return true;
    FILE* f = std::fopen(path, "r");
    if (!f) {
        std::fprintf(stderr, "core_gate_responder: cannot open CAPS file %s\n", path);
        return false;
    }
    char line[256]{};
    int lineno = 0;
    while (std::fgets(line, sizeof line, f)) {
        ++lineno;
        char* s = trim(line);
        if (*s == 0 || *s == '#')
            continue;
        char* eq = std::strchr(s, '=');
        if (!eq) {
            std::fprintf(stderr, "core_gate_responder: %s:%d: expected key=value\n", path, lineno);
            std::fclose(f);
            return false;
        }
        *eq = 0;
        char* key = trim(s);
        char* value = trim(eq + 1);
        uint32_t v = 0;
        if (!parseU32(value, v)) {
            std::fprintf(stderr, "core_gate_responder: %s:%d: bad integer '%s'\n", path, lineno, value);
            std::fclose(f);
            return false;
        }
        if (std::strcmp(key, "features") == 0) {
            caps.features = v;
        } else if (std::strcmp(key, "max_width") == 0) {
            if (v > 0xFFFFu) {
                std::fprintf(stderr, "core_gate_responder: %s:%d: max_width out of range\n", path, lineno);
                std::fclose(f);
                return false;
            }
            caps.max_width = static_cast<uint16_t>(v);
        } else if (std::strcmp(key, "max_height") == 0) {
            if (v > 0xFFFFu) {
                std::fprintf(stderr, "core_gate_responder: %s:%d: max_height out of range\n", path, lineno);
                std::fclose(f);
                return false;
            }
            caps.max_height = static_cast<uint16_t>(v);
        } else if (std::strcmp(key, "au_limit") == 0) {
            caps.au_limit = v;
        } else {
            std::fprintf(stderr, "core_gate_responder: %s:%d: unknown CAPS key '%s'\n", path, lineno, key);
            std::fclose(f);
            return false;
        }
    }
    std::fclose(f);
    return true;
}

// One injection line per new sequence number.
void applyAudio(const char* path, long& seq, fake_core::FakePlexCore& core) {
    if (!path)
        return;
    FILE* f = std::fopen(path, "r");
    if (!f)
        return;
    char line[256]{};
    const bool ok = std::fgets(line, sizeof line, f) != nullptr;
    std::fclose(f);
    if (!ok)
        return;
    long s = -1;
    char cmd[32]{};
    int used = 0;
    if (std::sscanf(line, "%ld %31s%n", &s, cmd, &used) < 2 || s == seq)
        return;
    seq = s;
    const char* rest = line + used;
    if (std::strcmp(cmd, "ignore") == 0) {
        core.audioIgnoreNext(static_cast<int>(std::strtol(rest, nullptr, 10)));
    } else if (std::strcmp(cmd, "reject") == 0) {
        core.audioRejectNext(static_cast<int>(std::strtol(rest, nullptr, 10)));
    } else if (std::strcmp(cmd, "wedge") == 0) {
        core.audioWedge(true);
    } else if (std::strcmp(cmd, "unwedge") == 0) {
        core.audioWedge(false);
    } else if (std::strcmp(cmd, "wedgeatprobe") == 0) {
        core.audioWedgeAtNextProbe();
    } else if (std::strcmp(cmd, "silent") == 0) {
        core.laneSilent();
    } else if (std::strcmp(cmd, "creset") == 0) {
        core.audioConsumerReset();
    } else if (std::strcmp(cmd, "cap") == 0) {
        core.audioConsumerCap(std::strtol(rest, nullptr, 10) != 0);
    } else if (std::strcmp(cmd, "orphan") == 0) {
        unsigned long long epoch = 0, nonce = 0, token = 0;
        int playing = 0;
        if (std::sscanf(rest, "%llx %llx %llu %d", &epoch, &nonce, &token, &playing) == 4)
            core.audioOrphan(epoch, nonce, token, playing != 0);
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "usage: %s FAKEMEM COUNTFILE BUILDFILE [AUDIOFILE]\n", argv[0]);
        return 2;
    }
    const char* capsFile = argc == 5 && looksLikeCapsFile(argv[4]) ? argv[4] : nullptr;
    const char* audioFile = argc == 5 && !capsFile ? argv[4] : nullptr;
    const int fd = ::open(argv[1], O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return 1;
    void* r = ::mmap(nullptr, 0x01000000u, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                     fake_core::kRegBase);
    void* w = ::mmap(nullptr, fake_core::kRingWindowLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                     fake_core::kRingWindowBase);
    void* p = nullptr;
    if (const char* e = std::getenv("FAKE_CORE_PLX"); e && std::strcmp(e, "1") == 0) {
        p = ::mmap(nullptr, 0x1000u, PROT_READ | PROT_WRITE, MAP_SHARED, fd, fake_core::kPlex480pPlxPage);
        if (p == MAP_FAILED)
            return 1;
    }
    ::close(fd);
    if (r == MAP_FAILED || w == MAP_FAILED)
        return 1;
    fake_core::FakePlexCore core(static_cast<volatile uint8_t*>(r),
                                 static_cast<volatile uint8_t*>(w),
                                 static_cast<volatile uint8_t*>(p));
    uint32_t build = readBuild(argv[3], 0);
    fake_core::FakePlexCore::CapsConfig caps;
    if (!loadCaps(capsFile, caps))
        return 2;
    core.setCapsConfig(caps);
    core.reprogram(build);
    long audioSeq = -1;
    std::signal(SIGTERM, onTerm);
    std::signal(SIGINT, onTerm);
    auto next = std::chrono::steady_clock::now();
    publish(argv[2], core);
    for (uint64_t i = 0; g_run.load(std::memory_order_relaxed); ++i) {
        core.step();
        if ((i & 0xFFFF) == 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next) {
                const uint32_t want = readBuild(argv[3], build);
                if (want != build) {
                    build = want;
                    if (!loadCaps(capsFile, caps))
                        return 2;
                    core.setCapsConfig(caps);
                    core.reprogram(build);
                    core.step(); // the new instance is configured before any injection
                }
                applyAudio(audioFile, audioSeq, core);
                publish(argv[2], core);
                next = now + std::chrono::milliseconds(20);
            }
        }
    }
    publish(argv[2], core);
    return 0;
}
