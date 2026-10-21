// review 06:57 D1 row: the R1 PLXH/PLXO path at daemon start, before any 480p present, on the real
// arm/misterplexd/fpga_spi.cpp in a host build with an unmanaged gate (MPX_CORE_GATE_START_OPEN) and
// MPX_DEVMEM_PATH a sparse 4 GiB fake memory file. The start-up frame map is the real one (320x240
// layout: phys 0x30000000 len 0x7f000, doorbell page 0x3007F000), which cannot reach PLXH 0x300FF138;
// on a lab MiSTer (record 0dc29063) the first idle flip was refused there and HDMI stayed black for ~40 s.
// An RTL stand-in thread maps the same fake page and does what ddr_frame_store.sv:1997-2012 and
// plex_fb480.sv:331-350 do: a free-running PLXH poll (the first good magic is the snap, a later new
// token is accepted), registration at each FB_VBL (60 Hz) with fb_live 0 at idle, and a change-only
// PLXO. Required: the first idle request after start is accepted, lands in the PLX page, and is ACKed
// (PLXO token, idle_shown=1, the requested buffer) within 400 ms of start; a second request too.
// Mutant MUT_R1_PLX_LAYOUT_PAGE (the pre-D1 path through the layout's mailbox page) must FAIL.
#include "fpga_spi.hpp"
#include "libmisterplex/r1_plxh.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
using misterplex::r1::kPlxhMagic;
using misterplex::r1::kPlxhPhys;
using misterplex::r1::kPlxoMagic;
using misterplex::r1::kPlxoPhys;
using Clock = std::chrono::steady_clock;

int g_fails = 0;
int g_checks = 0;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_fails;                                                                             \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                                       \
            std::printf(__VA_ARGS__);                                                              \
            std::printf("\n");                                                                     \
        }                                                                                          \
    } while (0)

bool makeSparse(const char* path, off_t size) {
    const int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    const bool ok = ::ftruncate(fd, size) == 0;
    ::close(fd);
    return ok;
}

std::string captureStderr(const std::function<void()>& fn) {
    const std::string path = std::string(MPX_DEVMEM_PATH) + ".stderr";
    std::fflush(stderr);
    const int saved = ::dup(2);
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (saved < 0 || fd < 0)
        return {};
    ::dup2(fd, 2);
    fn();
    std::fflush(stderr);
    ::dup2(saved, 2);
    ::close(saved);
    std::string out;
    char buf[4096];
    ::lseek(fd, 0, SEEK_SET);
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof buf)) > 0)
        out.append(buf, static_cast<size_t>(n));
    ::close(fd);
    ::unlink(path.c_str());
    return out;
}

// media_player.hpp FpgaSpiPlxPort, minus the CoreGate dependency (the gate is unmanaged here).
class FpgaPort final : public misterplex::r1::PlxPort {
public:
    explicit FpgaPort(misterplex::FpgaSpi& f) : f_(f) {}
    bool store32(uint32_t phys, uint32_t value) override { return f_.plxStore32(phys, value); }
    bool store64(uint32_t phys, uint64_t value) override { return f_.plxStore64(phys, value); }
    bool load64(uint32_t phys, uint64_t* value) override { return f_.plxLoad64(phys, value); }
    void barrier() override { __sync_synchronize(); }

private:
    misterplex::FpgaSpi& f_;
};

// The RTL's side of the PLX page, on the same fake memory.
class RtlStandIn {
public:
    explicit RtlStandIn(volatile uint32_t* page) : page_(page), th_([this] { loop(); }) {}
    ~RtlStandIn() {
        run_.store(false);
        th_.join();
    }

private:
    void loop() {
        bool armed = false, haveAcc = false, hEn = false, hSel = false;
        uint16_t snap = 0, acc = 0, hTok = 0;
        uint64_t pubKey = ~0ull;
        auto nextVbl = Clock::now() + std::chrono::microseconds(16667);
        while (run_.load()) {
            const uint32_t lo = page_[(kPlxhPhys & 0xFFFu) / 4], hi = page_[(kPlxhPhys & 0xFFFu) / 4 + 1];
            if (lo == kPlxhMagic) {
                const uint16_t tok = static_cast<uint16_t>(hi >> 16);
                if (!armed) {
                    snap = tok;
                    armed = true;
                } else if (tok != (haveAcc ? acc : snap)) {
                    acc = tok;
                    haveAcc = true;
                    hEn = (hi & 1u) != 0;
                    hSel = (hi & 2u) != 0;
                    hTok = tok;
                }
            }
            if (Clock::now() >= nextVbl) {   // FB_VBL: register, then publish change-only
                nextVbl += std::chrono::microseconds(16667);
                const bool idleSel = hEn;   // fb_live 0 at idle
                const bool ibuf = idleSel && hSel;
                const uint64_t key = (idleSel ? 1u : 0u) | (ibuf ? 2u : 0u) | (uint64_t(hTok) << 16);
                if (key != pubKey) {
                    pubKey = key;
                    page_[(kPlxoPhys & 0xFFFu) / 4 + 1] =
                        (uint32_t(hTok) << 16) | (idleSel ? 0u : 4u) | (ibuf ? 2u : 0u) | (idleSel ? 1u : 0u);
                    page_[(kPlxoPhys & 0xFFFu) / 4] = kPlxoMagic;
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));   // the ~2.9 us poll, slower
        }
    }

    volatile uint32_t* page_;
    std::atomic<bool> run_{true};
    std::thread th_;
};

bool ackedWithin(misterplex::r1::PlxhLink& link, uint16_t tok, int buf, Clock::time_point since, int ms,
                 double* tookMs) {
    while (Clock::now() - since < std::chrono::milliseconds(ms)) {
        if (link.ackedIdle(tok, buf)) {
            *tookMs = std::chrono::duration<double, std::milli>(Clock::now() - since).count();
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
}  // namespace

int main() {
    if (!makeSparse(MPX_DEVMEM_PATH, static_cast<off_t>(1) << 32)) {
        std::printf("test_r1_plx_page: cannot create the fake memory file\n");
        return 1;
    }
    {
        misterplex::FpgaSpi f;
        const auto start = Clock::now();
        // Daemon start: the frame map is the start-up 320x240 window, as on a lab MiSTer (record 0dc29063).
        const std::string mapLine = captureStderr([&] {
            misterplex::PlaybackCommand cmd;
            (void)f.readInputMailbox(cmd);
        });
        CHECK(mapLine.find("ddr_frame_map path=devmem wc_guard=0 phys=0x30000000 len=0x7f000") != std::string::npos,
              "the start-up frame map is not the len=0x7f000 window: %s", mapLine.c_str());

        const int fd = ::open(MPX_DEVMEM_PATH, O_RDWR | O_CLOEXEC);
        void* p = fd < 0 ? MAP_FAILED
                         : ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                                  static_cast<off_t>(kPlxhPhys & ~0xFFFu));
        if (p == MAP_FAILED) {
            std::printf("test_r1_plx_page: cannot map the fake PLX page\n");
            return 1;
        }
        volatile uint32_t* page = static_cast<volatile uint32_t*>(p);
        {
            RtlStandIn rtl(page);
            FpgaPort port(f);
            std::string primeLine;
            misterplex::r1::PlxhLink link(port, 0, [&](const std::string& l) { primeLine = l; });
            misterplex::r1::PlxhSessionPolicy policy(link);

            uint16_t tok = 0;
            std::string why;
            const bool accepted = policy.requestIdle(0, &tok, &why);
            CHECK(accepted, "the first idle request after start was refused: %s (%s)", why.c_str(),
                  f.lastError().c_str());
            CHECK(page[(kPlxhPhys & 0xFFFu) / 4] == kPlxhMagic && (page[(kPlxhPhys & 0xFFFu) / 4 + 1] >> 16) == tok &&
                      (page[(kPlxhPhys & 0xFFFu) / 4 + 1] & 1u) == 1u,
                  "PLXH 0x300FF138 does not hold token %u en=1 (lo=0x%08x hi=0x%08x)", tok,
                  page[(kPlxhPhys & 0xFFFu) / 4], page[(kPlxhPhys & 0xFFFu) / 4 + 1]);
            double took = -1;
            const bool acked = accepted && ackedWithin(link, tok, 0, start, 400, &took);
            CHECK(acked, "no idle ACK (PLXO token %u idle_shown=1 buf 0) within 400 ms of start", tok);
            std::printf("start: map len=0x7f000, %s; first idle request token %u ACKed %.1f ms after start\n",
                        primeLine.c_str(), tok, took);

            uint16_t tok2 = 0;
            const auto t2 = Clock::now();
            const bool accepted2 = policy.requestIdle(1, &tok2, &why);
            double took2 = -1;
            CHECK(accepted2 && ackedWithin(link, tok2, 1, t2, 400, &took2),
                  "a second idle request (buffer 1) was not ACKed within 400 ms: %s", why.c_str());
            std::printf("second idle request token %u (buffer 1) ACKed in %.1f ms\n", tok2, took2);
        }
        ::munmap(p, 4096);
        ::close(fd);
    }
    ::unlink(MPX_DEVMEM_PATH);
    if (g_fails) {
        std::printf("test_r1_plx_page: %d FAIL of %d checks\n", g_fails, g_checks);
        return 1;
    }
    std::printf("test_r1_plx_page: OK (%d checks; start-up len=0x7f000 map, PLXH/PLXO on their own page, "
                "idle ACK within 400 ms of start)\n",
                g_checks);
    return 0;
}
