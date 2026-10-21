// D52-LOG (lab, MPX_D52_LOG=1). Covers:
//   A. env parsing and defaults; the retired MPX_D52_LOG_PLX_PAGE is not read, and the note says so;
//   A2. the session's PLX page (sessionPlxPage) on the daemon's own DDR layouts (a real FpgaSpi): 0x48005D45,
//      0x48005D52 and 0x48009E36 at 640x480 -> 0x300FF000; every other FPGA build table row (0x2400F40B, 0x4801F40A, ...), an
//      unknown ID or 0 -> none, 'not a D52-LOG build' (S1); no layout or an unaligned base -> none;
//   B. the pure logger, with a fake clock: arm (with its page and build)/baseline, PLXJ classification, PLXD
//      gap/stall/jump, PLXF underrun/debug/liveness, absent-disarm, MVPS change-only, heartbeat n/a fields, the
//      line prefix; B2. no page: no arm and the loud line; samples and disarms of another page are dropped;
//   C. PlxPage on a fake /dev/mem file: decodes the three words, needs the magics, never writes, follows the page; reader refusals and detach;
//   D. off by default: no instance, no thread, no line (forked child);
//   E. the real poller thread end to end (forked child, MPX_D52_LOG=1, fake /dev/mem): arm on the 480p page, a
//      0x2400F40B BEGIN (no arm, the loud line), then a session on another page (the poller follows it);
//   F. the hooks in the real fpga_spi.cpp (forked child): BEGIN published + fence, epoch reset, PLXA result;
//   G. the real CAPS -> BEGIN -> arm -> END path (fpga_spi.cpp, forked child, an in-process fake Plex core that
//      keeps the 480p PLX page): 0x2400F40B and 0x4801F40A -> no arm, the loud line; 0x48005D52 -> arm at
//      0x300ff000, a live baseline, 'ring END fence=ok' and 'disarm (END)'.
#include "fake_plex_core.hpp"
#include "fpga_spi.hpp"
#include "libmisterplex/d52_log.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef MPX_DEVMEM_PATH
#error "build with -DMPX_DEVMEM_PATH=\"<fake /dev/mem file>\""
#endif

using namespace misterplex;

namespace {
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

uint64_t plxj(unsigned x, unsigned y, unsigned t) {
    return d52::kPlxjMagic | (uint64_t(x & 0xFFF) << 32) | (uint64_t(y & 0xFFF) << 44) | (uint64_t(t & 0xFF) << 56);
}
uint64_t plxd(unsigned fd, unsigned pending = 0, unsigned bank = 0) {
    return d52::kPlxdMagic | (uint64_t(pending & 1) << 35) | (uint64_t(bank & 1) << 34) | (uint64_t(fd & 0xFFFF) << 48);
}
uint64_t plxf(unsigned seq, unsigned debug, unsigned underrun) {
    return d52::kPlxfMagic | (uint64_t(seq & 0xFF) << 32) | (uint64_t(debug & 0xFF) << 40) |
           (uint64_t(underrun & 0xFFFF) << 48);
}

struct Rig {
    int64_t now_us = 1000000000; // mono 1000 s
    std::vector<std::string> lines;
    d52::Logger log;
    explicit Rig(d52::Config c = d52::Config{})
        : log(c, [this](const std::string& s) { lines.push_back(s); },
              [this] { return d52::Stamp{now_us, now_us + 1700000000000000LL}; }) {}
    d52::PlxWords w(uint64_t j, uint64_t d, uint64_t f) const {
        d52::PlxWords s;
        s.j_ok = s.d_ok = s.f_ok = true;
        s.j = j;
        s.d = d;
        s.f = f;
        return s;
    }
    size_t count(const char* needle) const {
        size_t n = 0;
        for (const auto& l : lines)
            n += l.find(needle) != std::string::npos;
        return n;
    }
    std::string last() const { return lines.empty() ? std::string() : lines.back(); }
};

bool contains(const std::string& s, const char* n) { return s.find(n) != std::string::npos; }

std::string readFile(const std::string& p) {
    std::string out;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f)
        return out;
    char b[4096];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0)
        out.append(b, n);
    std::fclose(f);
    return out;
}

bool pwrite64At(const char* path, uint64_t phys, uint64_t value) {
    const int fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool ok = ::pwrite(fd, &value, 8, static_cast<off_t>(phys)) == 8;
    ::close(fd);
    return ok;
}

std::string preadPage(const char* path, uint64_t phys) {
    std::string out(4096, '\0');
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return {};
    const bool ok = ::pread(fd, &out[0], out.size(), static_cast<off_t>(phys)) == 4096;
    ::close(fd);
    return ok ? out : std::string();
}

// A test reader for PlxPage (sections C and E): pread of the fake memory file, no map. The daemon's reader is
// FpgaSpi::plxPageRead32 (section G and test_d52_plx_mapper).
const int kOwnerE = 0; // section E's reader owner tag

d52::PlxPage::Reader fileReader(const char* path) {
    return [path](uint32_t page, uint32_t off, uint32_t& v) {
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return d52::PlxRead::Unavailable;
        const bool ok = ::pread(fd, &v, 4, static_cast<off_t>(page) + off) == 4;
        ::close(fd);
        return ok ? d52::PlxRead::Ok : d52::PlxRead::Unavailable;
    };
}

// Runs fn in a child with stderr captured; returns the child's stderr and sets rc.
template <class Fn> std::string inChild(const char* tag, Fn fn, int& rc) {
    const std::string cap = std::string(MPX_DEVMEM_PATH) + "." + tag + ".stderr";
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        const int fd = ::open(cap.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0)
            ::dup2(fd, 2);
        const int r = fn();
        std::fflush(stderr);
        ::_exit(r);
    }
    int st = 0;
    ::waitpid(pid, &st, 0);
    rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    std::string out = readFile(cap);
    ::unlink(cap.c_str());
    return out;
}
} // namespace

int main() {
    // ---- A. env ----
    ::unsetenv(d52::kEnv);
    ::unsetenv(d52::kPollEnv);
    ::unsetenv(d52::kGapEnv);
    ::unsetenv(d52::kRetiredPageEnv);
    {
        auto c = d52::configFromEnv();
        CHECK(!c.enabled && c.poll_ms == 5 && c.gap_ms == 60 && !*c.note, "defaults");
        for (const char* v : {"0", "yes", "true", "01", " 1", "2"}) {
            ::setenv(d52::kEnv, v, 1);
            CHECK(!d52::configFromEnv().enabled, "'%s' does not enable", v);
        }
        ::setenv(d52::kEnv, "1", 1);
        CHECK(d52::configFromEnv().enabled, "1 enables");
        ::setenv(d52::kPollEnv, "2", 1);
        ::setenv(d52::kGapEnv, "45", 1);
        c = d52::configFromEnv();
        CHECK(c.poll_ms == 2 && c.gap_ms == 45 && !*c.note, "in-range values");
        ::setenv(d52::kRetiredPageEnv, "3007F000", 1);
        c = d52::configFromEnv();
        CHECK(c.poll_ms == 2 && c.gap_ms == 45 && contains(c.note, "MPX_D52_LOG_PLX_PAGE is not read"),
              "the retired page env is not read, and the note says so: '%s'", c.note);
        ::unsetenv(d52::kRetiredPageEnv);
        ::setenv(d52::kPollEnv, "0", 1);
        ::setenv(d52::kGapEnv, "x", 1);
        c = d52::configFromEnv();
        CHECK(c.poll_ms == 5 && c.gap_ms == 60 && *c.note, "out of range: defaults + note");
        ::unsetenv(d52::kEnv);
        ::unsetenv(d52::kPollEnv);
        ::unsetenv(d52::kGapEnv);
    }

    // ---- A2. the session's PLX page, on the daemon's own layouts ----
    {
        // setSourceAspect's layout (media_player.cpp: setDdrFrameLayout(ddrFrameGeometryForPresentedSize(outW_,
        // outH_))) at the 480p conf's DECODE=640x480, on a real FpgaSpi (no device is opened).
        FpgaSpi f;
        const bool set = f.setDdrFrameLayout(ddrFrameGeometryForPresentedSize(640, 480), DdrFrameFormat::Yuv420p);
        const DdrFrameLayout l = f.ddrFrameLayout();
        CHECK(set && l.doorbell_phys == 0x300FF000u,
              "640x480: the daemon's doorbell is the 480p RTL page 0x300FF000 (got 0x%08x)", l.doorbell_phys);
        const char* why = "unset";
        CHECK(d52::sessionPlxPage(0x48005D52u, l.doorbell_phys, l.phys_base, &why) == 0x300FF000u && !why,
              "0x48005D52 -> 0x300FF000");
        CHECK(d52::sessionPlxPage(0x48005D45u, l.doorbell_phys, l.phys_base, &why) == 0x300FF000u && !why,
              "0x48005D45 -> 0x300FF000");
        CHECK(d52::sessionPlxPage(0x48009E36u, l.doorbell_phys, l.phys_base, &why) == 0x300FF000u && !why,
              "0x48009E36 -> 0x300FF000");
        // S1 (review 00:52/00:55; 9E36 added review 14:13 item 6): only the three D52-LOG builds arm. Every other row of the FPGA build table, the
        // 240p 0x2400F40B and the 480i 0x4801F40A among them, and any unknown ID: no page, 'not a D52-LOG build'.
        int d52Rows = 0, otherRows = 0, otherNoPage = 0;
        for (const FpgaBuildInfo& b : kRecognisedFpgaBuilds) {
            const uint32_t pg = d52::sessionPlxPage(b.id, l.doorbell_phys, l.phys_base, &why);
            if (b.id == 0x48005D45u || b.id == 0x48005D52u || b.id == 0x48009E36u) {
                ++d52Rows;
                CHECK(pg == 0x300FF000u && !why && std::strcmp(b.raster, "480p") == 0, "D52 row 0x%08X", b.id);
            } else {
                ++otherRows;
                const bool none = pg == 0 && why && std::strcmp(why, "not a D52-LOG build") == 0;
                otherNoPage += none;
                CHECK(none, "table row 0x%08X (%s): no page, 'not a D52-LOG build' (got 0x%08x)", b.id, b.raster, pg);
            }
        }
        CHECK(d52Rows == 3 && otherRows >= 10 && otherNoPage == otherRows,
              "all three D52 builds are table rows; %d other rows, %d without a page", otherRows, otherNoPage);
        CHECK(recognisedFpgaBuild(0x2400F40Bu) && recognisedFpgaBuild(0x4801F40Au),
              "0x2400F40B and 0x4801F40A are table rows (the allowlist is narrower than the table)");
        for (const uint32_t id : {0x2400F40Bu, 0x4801F40Au, 0x48005D50u, 0x48005DFFu, 0x12345678u, 0u}) {
            const uint32_t pg = d52::sessionPlxPage(id, l.doorbell_phys, l.phys_base, &why);
            CHECK(pg == 0 && why && std::strcmp(why, "not a D52-LOG build") == 0, "0x%08X: no page (got 0x%08x)", id, pg);
        }
        CHECK(d52::sessionPlxPage(0x48005D52u, 0, 0, &why) == 0 && why && contains(why, "no resolved DDR doorbell base"),
              "no layout: no page");
        CHECK(d52::sessionPlxPage(0x48005D52u, 0x2FFFF000u, 0x30000000u, &why) == 0 && why &&
                  contains(why, "no resolved DDR doorbell base"),
              "a doorbell below its base: no page");
        CHECK(d52::sessionPlxPage(0x48005D52u, 0x300FF010u, 0x30000000u, &why) == 0 && why &&
                  contains(why, "page-aligned"),
              "an unaligned doorbell: no page");
        // FpgaSpi's layout before any setSourceAspect is 320x240: doorbell 0x3007F000, the page D52-LOG once read
        // for every build.
        FpgaSpi fresh;
        CHECK(fresh.ddrFrameLayout().doorbell_phys == 0x3007F000u, "the default (320x240) doorbell is 0x3007F000 (got 0x%08x)",
              fresh.ddrFrameLayout().doorbell_phys);
    }

    // ---- B. the logger ----
    {
        Rig r;
        r.log.sample(r.w(plxj(4, 3, 7), plxd(100), plxf(1, 0, 0)));
        CHECK(r.lines.empty(), "no sample before arm");
        r.log.arm(0x1234, 0x300FF000u, 0x48005D52u);
        CHECK(r.count("arm session=0x1234 plx_page=0x300ff000 source=doorbell build=0x48005D52") == 1, "arm line: %s",
              r.last().c_str());
        CHECK(r.log.armed() && r.log.page() == 0x300FF000u, "armed on its page");
        r.log.sample(r.w(plxj(4, 3, 7), plxd(100), plxf(1, 0, 0)));
        CHECK(contains(r.last(), "plx baseline plxj=4:3/7 plxd frames_done=100 swap_pending=0 disp_bank=0 plxf underrun=0 debug=0x00"),
              "baseline: %s", r.last().c_str());
        CHECK(r.last().rfind("d52 t=1000.000000 rt=1700001000.000000 ", 0) == 0, "prefix: %s", r.last().c_str());
        const size_t base = r.lines.size();
        // 33 ms frames, PLXF seq heartbeat: no lines.
        unsigned fd = 100, seq = 1;
        for (int i = 0; i < 30; ++i) {
            r.now_us += 33333;
            r.log.sample(r.w(plxj(4, 3, 7), plxd(++fd, i & 1, i & 1), plxf(++seq, 0, 0)));
        }
        CHECK(r.lines.size() == base, "steady 30p: no lines (%zu new)", r.lines.size() - base);
        // A 100 ms gap.
        r.now_us += 50000;
        r.log.sample(r.w(plxj(4, 3, 7), plxd(fd), plxf(++seq, 0, 0)));
        r.now_us += 50000;
        r.log.sample(r.w(plxj(4, 3, 7), plxd(fd + 1, 1, 1), plxf(++seq, 0, 0)));
        ++fd;
        CHECK(r.count("plxd gap ms=100 frames_done 130 -> 131 swap_pending=1 disp_bank=1 since_t=") == 1, "gap line: %s",
              r.last().c_str());
        // A stall: one line at >= 1000 ms, then the gap line when it ends.
        for (int i = 0; i < 12; ++i) {
            r.now_us += 100000;
            r.log.sample(r.w(plxj(4, 3, 7), plxd(fd), plxf(++seq, 0, 0)));
        }
        CHECK(r.count("plxd stall ms=1000 frames_done=131") == 1, "stall once");
        r.now_us += 100000;
        r.log.sample(r.w(plxj(4, 3, 7), plxd(fd + 1), plxf(++seq, 0, 0)));
        ++fd;
        CHECK(r.count("plxd gap ms=1300 frames_done 131 -> 132") == 1, "gap after stall: %s", r.last().c_str());
        // Backwards = jump.
        r.now_us += 33333;
        r.log.sample(r.w(plxj(4, 3, 7), plxd(5), plxf(++seq, 0, 0)));
        fd = 5;
        CHECK(r.count("plxd frames_done jump 132 -> 5") == 1, "jump");
        // Wrap 65535 -> 0 is an advance, not a jump.
        {
            Rig w;
            w.log.arm(1, 0x300FF000u, 0x48005D52u);
            w.log.sample(w.w(plxj(4, 3, 1), plxd(65535), plxf(1, 0, 0)));
            w.now_us += 33333;
            w.log.sample(w.w(plxj(4, 3, 1), plxd(0), plxf(2, 0, 0)));
            CHECK(w.count("jump") == 0 && w.count("gap") == 0, "wrap is an advance");
        }
        // PLXJ: the daemon's own logged download.
        const uint8_t pkt[9] = {0x50, 0x4C, 0x58, 0x41, 15, 0, 11, 0, 8}; // 0x41584C50 'PLXA' LE, 15:11, token 8
        r.log.plxaDownload(pkt, sizeof pkt);
        CHECK(contains(r.last(), "plxa_download index=4 len=9 magic=PLXA plxa=15:11 token=8"), "download line: %s",
              r.last().c_str());
        r.now_us += 5000;
        r.log.sample(r.w(plxj(15, 11, 8), plxd(fd), plxf(++seq, 0, 0)));
        CHECK(contains(r.last(), "plxj change 4:3/7 -> 15:11/8 src=logged_plxa_download"), "logged: %s", r.last().c_str());
        // last+1 with no logged download.
        r.now_us += 5000;
        r.log.sample(r.w(plxj(15, 11, 9), plxd(fd), plxf(++seq, 0, 0)));
        CHECK(contains(r.last(), "plxj change 15:11/8 -> 15:11/9 src=token_last+1_NO_logged_download"), "last+1: %s",
              r.last().c_str());
        // Any other token.
        r.now_us += 5000;
        r.log.sample(r.w(plxj(4, 3, 200), plxd(fd), plxf(++seq, 0, 0)));
        CHECK(contains(r.last(), "plxj change 15:11/9 -> 4:3/200 src=other_token_NO_logged_download"), "other: %s",
              r.last().c_str());
        // PLXJ unchanged: nothing.
        size_t n0 = r.lines.size();
        r.now_us += 5000;
        r.log.sample(r.w(plxj(4, 3, 200), plxd(fd), plxf(++seq, 0, 0)));
        CHECK(r.lines.size() == n0, "unchanged plxj: no line");
        // PLXF: underrun at most 1/s against the last logged state, debug at once. (count(): a PLXD stall line can
        // come from the same sample.)
        r.now_us += 5000;
        r.log.sample(r.w(plxj(4, 3, 200), plxd(fd), plxf(++seq, 0, 1)));
        CHECK(r.count("plxf underrun 0 -> 1 debug 0x00 -> 0x00") == 1, "underrun 0 -> 1");
        r.now_us += 5000;
        r.log.sample(r.w(plxj(4, 3, 200), plxd(fd), plxf(++seq, 0, 2)));
        CHECK(r.count("plxf underrun 1 -> 2") == 0, "second underrun within 1 s: held");
        r.now_us += 1000000;
        r.log.sample(r.w(plxj(4, 3, 200), plxd(fd), plxf(++seq, 0, 2)));
        CHECK(r.count("plxf underrun 1 -> 2 debug 0x00 -> 0x00") == 1, "trailing underrun after 1 s");
        r.now_us += 5000;
        r.log.sample(r.w(plxj(4, 3, 200), plxd(fd), plxf(++seq, 0xE1, 2)));
        CHECK(r.count("plxf underrun 2 -> 2 debug 0x00 -> 0xe1") == 1, "debug at once");
        // PLXF stale then live.
        for (int i = 0; i < 11; ++i) {
            r.now_us += 100000;
            r.log.sample(r.w(plxj(4, 3, 200), plxd(fd), plxf(seq, 0xE1, 2)));
        }
        CHECK(r.count("plxf stale (seq unchanged 1000 ms)") == 1, "plxf stale once");
        r.now_us += 5000;
        r.log.sample(r.w(plxj(4, 3, 200), plxd(fd), plxf(++seq, 0xE1, 2)));
        CHECK(r.count("plxf live (seq advancing again)") == 1, "plxf live");
        // MVPS: change only.
        r.log.mvps(0x1234, true, true, false, 0, 10, 9);
        CHECK(contains(r.last(), "mvps session=0x1234 active=1 has_frame=1 error=0 code=0 count=10 seq=9"), "mvps first");
        n0 = r.lines.size();
        r.log.mvps(0x1234, true, true, false, 0, 11, 10);
        CHECK(r.lines.size() == n0, "mvps count-only change: no line");
        r.log.mvps(0x1234, true, true, true, 0x27, 12, 11);
        CHECK(contains(r.last(), "error=1 code=39"), "mvps error code: %s", r.last().c_str());
        // Daemon-side causes.
        r.log.ringRecord("FLUSH", 0x1234, 4096);
        CHECK(contains(r.last(), "ring FLUSH published session=0x1234 count=4096"), "ring record");
        r.log.ringFence("FLUSH", 0x1234, true);
        CHECK(contains(r.last(), "ring FLUSH fence=ok session=0x1234"), "ring fence");
        r.log.statusFlush(true);
        CHECK(contains(r.last(), "status FLUSH status[11] pulse result=ok"), "status flush");
        r.log.ringReset("EPOCH_RESET (flushBitstreamDdr)", 1, "published");
        CHECK(contains(r.last(), "ring EPOCH_RESET (flushBitstreamDdr) epoch=1 result=published"), "epoch reset");
        // Heartbeat: armed, counters, the n/a fields.
        r.log.tick();
        r.now_us += 60000000;
        r.log.tick();
        CHECK(contains(r.last(), "hb armed=1 session=0x1234") && contains(r.last(), "plxj=4:3/200") &&
                  contains(r.last(), "has_frame=n/a(spi-only) fb_par=n/a(uio-only) visible=n/a r2_out=n/a frame=640x480(const)"),
              "heartbeat: %s", r.last().c_str());
        // Absent 5 s: disarm, then silence.
        d52::PlxWords none;
        for (int i = 0; i < 6; ++i) {
            r.now_us += 1000000;
            r.log.sample(none);
        }
        CHECK(r.count("disarm (PLX page absent") == 1 && !r.log.armed(), "absent 5 s disarms");
        n0 = r.lines.size();
        r.log.sample(r.w(plxj(1, 1, 1), plxd(1), plxf(1, 0, 0)));
        r.log.disarm("again");
        CHECK(r.lines.size() == n0, "disarmed: no sample lines, no second disarm");
        // Every line has the prefix.
        size_t bad = 0;
        for (const auto& l : r.lines)
            bad += l.rfind("d52 t=", 0) != 0 || l.find(" rt=") == std::string::npos;
        CHECK(bad == 0, "%zu lines without the prefix", bad);
    }

    // ---- B2. no page: no arm and the loud line; another page's samples and disarms are dropped ----
    {
        Rig r;
        r.log.armRefused(0x99, 0x2400F40Bu, "not a D52-LOG build");
        CHECK(r.count("d52 not armed: no PLX page for build 0x2400F40B (not a D52-LOG build) session=0x99") == 1,
              "the loud line: %s", r.last().c_str());
        CHECK(!r.log.armed() && r.log.page() == 0 && r.count("disarm (") == 0, "not armed, nothing to disarm");
        r.log.sampleFrom(0, r.w(plxj(4, 3, 7), plxd(1), plxf(1, 0, 0)));
        CHECK(r.lines.size() == 1, "not armed: a sample logs nothing");
        r.log.arm(0x100, 0x300FF000u, 0x48005D52u);
        r.log.sampleFrom(0x3007F000u, r.w(plxj(9, 9, 9), plxd(5), plxf(1, 0, 0)));
        CHECK(r.count("plx baseline") == 0, "a sample of another page is dropped");
        r.log.sampleFrom(0x300FF000u, r.w(plxj(4, 3, 7), plxd(1), plxf(1, 0, 0)));
        CHECK(r.count("plx baseline plxj=4:3/7 plxd frames_done=1") == 1, "the armed page's sample is the baseline");
        r.log.disarmPage(0x3007F000u, "a map failure on another page");
        CHECK(r.log.armed() && r.count("disarm (") == 0, "a disarm for another page leaves the session armed");
        r.log.armRefused(0x101, 0x2400F40Bu, "not a D52-LOG build");
        CHECK(r.count("disarm (superseded by a BEGIN with no PLX page)") == 1 && !r.log.armed() && r.log.page() == 0,
              "a BEGIN with no page ends the armed session");
        CHECK(r.count("d52 not armed: no PLX page for build 0x2400F40B") == 2, "one loud line per refused BEGIN");
    }

    // ---- C. PlxPage on a fake /dev/mem ----
    const char* mem = MPX_DEVMEM_PATH;
    {
        const int fd = ::open(mem, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        CHECK(fd >= 0 && ::ftruncate(fd, 0x40000000) == 0, "fake memory");
        if (fd >= 0)
            ::close(fd);
        const uint32_t pg = 0x300FF000u; // the 480p page (A2)
        CHECK(pwrite64At(mem, pg + d52::kPlxjOff, plxj(4, 3, 5)) && pwrite64At(mem, pg + d52::kPlxdOff, plxd(77, 1, 1)) &&
                  pwrite64At(mem, pg + d52::kPlxfOff, plxf(3, 0, 9)),
              "seed words");
        const std::string before = preadPage(mem, pg);
        d52::PlxPage page;
        d52::PlxRead st = d52::PlxRead::Ok;
        d52::PlxWords w = page.read(pg, st);
        CHECK(st == d52::PlxRead::Unavailable && !w.j_ok && !w.d_ok && !w.f_ok, "no reader attached: no words");
        page.attach(&page, fileReader(mem));
        w = page.read(pg, st);
        CHECK(st == d52::PlxRead::Ok, "read through the reader");
        CHECK(w.j_ok && d52::Logger::jx(w.j) == 4 && d52::Logger::jy(w.j) == 3 && d52::Logger::jt(w.j) == 5, "plxj read");
        CHECK(w.d_ok && d52::Logger::fdOf(w.d) == 77 && d52::Logger::swapPendingOf(w.d) == 1, "plxd read");
        CHECK(w.f_ok && d52::Logger::underrunOf(w.f) == 9, "plxf read");
        CHECK(pwrite64At(mem, pg + d52::kPlxjOff, 0x0000000012345678ull), "bad magic");
        w = page.read(pg, st);
        CHECK(!w.j_ok && w.d_ok && w.f_ok, "a word without its magic is not ok");
        CHECK(pwrite64At(mem, pg + d52::kPlxjOff, plxj(4, 3, 5)), "restore");
        for (int i = 0; i < 100; ++i)
            (void)page.read(pg, st);
        CHECK(!before.empty() && preadPage(mem, pg) == before, "reads never write the page");
        // Another page is just another read (the reader's mapper decides whether it may read it).
        const uint32_t pg2 = 0x3007F000u;
        CHECK(pwrite64At(mem, pg2 + d52::kPlxdOff, plxd(500)), "seed page 2");
        w = page.read(pg2, st);
        CHECK(!w.j_ok && w.d_ok && d52::Logger::fdOf(w.d) == 500 && !w.f_ok, "page 2's words");
        CHECK(page.read(pg, st).j_ok && d52::Logger::fdOf(page.read(pg, st).d) == 77, "the first page again");
        // A refusal from the reader is passed on, and no word of that page is ok.
        int other_calls = 0;
        page.attach(&page, [&other_calls](uint32_t, uint32_t, uint32_t&) {
            ++other_calls;
            return d52::PlxRead::OtherPage;
        });
        w = page.read(pg, st);
        CHECK(st == d52::PlxRead::OtherPage && !w.j_ok && !w.d_ok && !w.f_ok && other_calls == 1,
              "OtherPage: passed on at the first load, nothing ok (%d calls)", other_calls);
        // Only the attached owner detaches.
        page.attach(&page, fileReader(mem));
        int stranger = 0;
        page.detach(&stranger);
        CHECK(page.read(pg, st).j_ok && st == d52::PlxRead::Ok, "another owner's detach is a no-op");
        page.detach(&page);
        w = page.read(pg, st);
        CHECK(st == d52::PlxRead::Unavailable && !w.j_ok, "detached: no reads");
    }

    // ---- D. off by default ----
    {
        int rc = -1;
        const std::string err = inChild("off", [] {
            ::unsetenv(d52::kEnv);
            if (d52::instance() != nullptr || d52::active() != nullptr)
                return 3;
            d52::armSession(0x55, 0x48005D52u, 0x300FF000u, 0x30000000u, nullptr, {}); // must not start anything
            ::usleep(20000);
            return 0;
        }, rc);
        CHECK(rc == 0, "off: no instance (rc %d)", rc);
        CHECK(err.find("d52 ") == std::string::npos, "off: no d52 line: %s", err.c_str());
    }

    // ---- E. the real poller thread ----
    {
        int rc = -1;
        const uint32_t pg = 0x300FF000u, pg2 = 0x3007F000u;
        CHECK(pwrite64At(mem, pg + d52::kPlxjOff, plxj(4, 3, 5)) && pwrite64At(mem, pg + d52::kPlxdOff, plxd(10)), "seed");
        CHECK(pwrite64At(mem, pg2 + d52::kPlxjOff, plxj(16, 9, 2)) && pwrite64At(mem, pg2 + d52::kPlxdOff, plxd(500)),
              "seed page 2");
        const std::string err = inChild("poller", [mem, pg, pg2] {
            ::setenv(d52::kEnv, "1", 1);
            ::setenv(d52::kPollEnv, "2", 1);
            if (!d52::active())
                return 3;
            d52::armSession(0x77, 0x48005D52u, pg, 0x30000000u, &kOwnerE, fileReader(mem));
            ::usleep(60000);
            (void)pwrite64At(mem, pg + d52::kPlxjOff, plxj(15, 11, 6)); // a stray: last+1, nothing logged
            ::usleep(60000);
            d52::armSession(0x78, 0x2400F40Bu, pg, 0x30000000u, &kOwnerE, fileReader(mem)); // 240p: not a D52-LOG build, no arm
            ::usleep(30000);
            d52::armSession(0x79, 0x48005D45u, pg2, 0x30000000u, &kOwnerE, fileReader(mem)); // another page: the poller follows
            ::usleep(60000);
            return 0;
        }, rc);
        CHECK(rc == 0, "poller child rc %d", rc);
        CHECK(contains(err, " start pid=") && contains(err, "poll_ms=2 gap_ms=60 stall_ms=1000 plx_page=at_arm"),
              "start line: %s", err.c_str());
        CHECK(contains(err, "arm session=0x77 plx_page=0x300ff000 source=doorbell build=0x48005D52"), "armed: %s",
              err.c_str());
        CHECK(contains(err, "plx baseline plxj=4:3/5 plxd frames_done=10"), "baseline from the map: %s", err.c_str());
        CHECK(contains(err, "plxj change 4:3/5 -> 15:11/6 src=token_last+1_NO_logged_download"), "live plxj change: %s",
              err.c_str());
        CHECK(contains(err, "disarm (superseded by a BEGIN with no PLX page)") &&
                  contains(err, "d52 not armed: no PLX page for build 0x2400F40B (not a D52-LOG build) session=0x78") &&
                  !contains(err, "arm session=0x78"),
              "0x2400F40B: no arm, the loud line: %s", err.c_str());
        CHECK(contains(err, "arm session=0x79 plx_page=0x3007f000 source=doorbell build=0x48005D45") &&
                  contains(err, "plx baseline plxj=16:9/2 plxd frames_done=500"),
              "the poller follows the new session's page: %s", err.c_str());
    }

    // ---- F. the hooks in the real fpga_spi.cpp ----
    {
        int rc = -1;
        const std::string err = inChild("spi", [] {
            ::setenv(d52::kEnv, "1", 1);
            FpgaSpi f;
            // PLXR (consumer count 0) so the BEGIN record can be written; nothing acknowledges it.
            (void)pwrite64At(MPX_DEVMEM_PATH, ddr_bitstream_ring::kReadPhys, ddr_bitstream_ring::kReadMagic);
            (void)f.beginBitstreamSession(0xBEEF, 20);
            (void)f.flushBitstreamDdr();
            // FpgaSpi::open() would map the FPGA register window from the same file (past its end here) and reach
            // SpiExclusive, which parks Main. Move the file aside so open() fails cleanly: no register access, and
            // the download fails before any transfer.
            const std::string away = std::string(MPX_DEVMEM_PATH) + ".away";
            if (::rename(MPX_DEVMEM_PATH, away.c_str()) != 0)
                return 4;
            SourceAspect a{16, 9, true};
            (void)f.sendSourceAspect(a);
            if (::rename(away.c_str(), MPX_DEVMEM_PATH) != 0)
                return 5;
            return 0;
        }, rc);
        CHECK(rc == 0, "spi child rc %d", rc);
        CHECK(contains(err, "ring BEGIN published session=0xbeef count="), "BEGIN published: %s", err.c_str());
        CHECK(contains(err, "ring BEGIN fence=failed session=0xbeef"), "BEGIN fence failed");
        CHECK(contains(err, "ring EPOCH_RESET (flushBitstreamDdr) epoch="), "epoch reset hook");
        CHECK(contains(err, "plxa_send source_dar=16:9 token=1 baseline=unread result=download_failed"),
              "PLXA result hook: %s", err.c_str());
        CHECK(!contains(err, "arm session=0xbeef"), "an unacknowledged BEGIN does not arm");
        CHECK(!contains(err, "d52 not armed"), "an unacknowledged BEGIN makes no arm decision");
    }

    // ---- G. the real CAPS -> BEGIN -> arm -> END path in fpga_spi.cpp, a fake Plex core per build ----
    {
        // The fake core (fake_plex_core.hpp, as in test_core_gate) steps in its own thread on this fake /dev/mem.
        // Its AU-ring window and its 480p PLX page are the file's, so the poller reads what it writes. Its SPI
        // registers are a private map (nothing here uses SPI). One FpgaSpi, as in the daemon: setSourceAspect's
        // 640x480 layout, then one session per build, the core reprogrammed before each. The arm decision takes
        // the build from the session's own CAPS reply.
        const uint32_t pg = 0x300FF000u;
        for (const uint32_t off : {d52::kPlxfOff, d52::kPlxdOff, d52::kPlxjOff})
            CHECK(pwrite64At(mem, pg + off, 0), "clear the 480p PLX page");
        int rc = -1;
        const std::string err = inChild("real", [mem] {
            ::setenv(d52::kEnv, "1", 1);
            ::setenv(d52::kPollEnv, "2", 1);
            const int fd = ::open(mem, O_RDWR | O_CLOEXEC);
            if (fd < 0)
                return 3;
            void* w = ::mmap(nullptr, fake_core::kRingWindowLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                             fake_core::kRingWindowBase);
            void* p = ::mmap(nullptr, 0x1000u, PROT_READ | PROT_WRITE, MAP_SHARED, fd, fake_core::kPlex480pPlxPage);
            ::close(fd);
            void* r = ::mmap(nullptr, 0x01000000u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (w == MAP_FAILED || p == MAP_FAILED || r == MAP_FAILED)
                return 4;
            fake_core::FakePlexCore core(static_cast<volatile uint8_t*>(r), static_cast<volatile uint8_t*>(w),
                                         static_cast<volatile uint8_t*>(p));
            core.reprogram(0x2400F40Bu);
            std::atomic<bool> run{true};
            std::thread t([&core, &run] {
                while (run.load(std::memory_order_relaxed))
                    core.step();
            });
            FpgaSpi f;
            int out = f.setDdrFrameLayout(ddrFrameGeometryForPresentedSize(640, 480), DdrFrameFormat::Yuv420p) ? 0 : 5;
            const uint32_t builds[] = {0x2400F40Bu, 0x4801F40Au, 0x48005D52u};
            for (int i = 0; i < 3 && !out; ++i) {
                core.reprogram(builds[i]); // a new core instance (atomics: safe from this thread)
                ::usleep(5000);
                FpgaSpi::VideoCapabilities caps;
                const uint64_t sid = 0xD5200001ull + static_cast<uint64_t>(i);
                if (!f.beginFpgaVideoSession(sid, caps, 2000))
                    out = 10 + i;
                else if (caps.build_id != builds[i])
                    out = 20 + i;
                ::usleep(80000); // armed: the poller's baseline from the core's page
                if (!out && !f.endBitstreamSession(sid, 2000))
                    out = 30 + i;
                ::usleep(20000);
            }
            if (out)
                std::fprintf(stderr, "G error %d: %s\n", out, f.lastError().c_str());
            run.store(false);
            t.join();
            return out;
        }, rc);
        CHECK(rc == 0, "real-path child rc %d: %s", rc, err.c_str());
        for (const char* s : {"0xd5200001", "0xd5200002", "0xd5200003"}) {
            CHECK(contains(err, (std::string("ring BEGIN published session=") + s).c_str()) &&
                      contains(err, (std::string("ring END fence=ok session=") + s).c_str()),
                  "session %s: BEGIN published, END fenced", s);
        }
        CHECK(contains(err, "d52 not armed: no PLX page for build 0x2400F40B (not a D52-LOG build) session=0xd5200001") &&
                  !contains(err, "arm session=0xd5200001"),
              "240p 0x2400F40B (CAPS, real BEGIN ack): no arm, the loud line: %s", err.c_str());
        CHECK(contains(err, "d52 not armed: no PLX page for build 0x4801F40A (not a D52-LOG build) session=0xd5200002") &&
                  !contains(err, "arm session=0xd5200002"),
              "480i 0x4801F40A: no arm, the loud line");
        const size_t armAt = err.find("arm session=0xd5200003 plx_page=0x300ff000 source=doorbell build=0x48005D52\n");
        const size_t endAt = err.find("disarm (END)");
        CHECK(armAt != std::string::npos, "0x48005D52 (CAPS): the arm line names 0x300ff000: %s", err.c_str());
        CHECK(contains(err, "plx baseline plxj=absent plxd frames_done=0 ") && contains(err, "plxf underrun=0 debug=0x00"),
              "0x48005D52: the baseline is the fake core's live 480p page");
        size_t ends = 0;
        for (size_t at = err.find("disarm (END)"); at != std::string::npos; at = err.find("disarm (END)", at + 1))
            ++ends;
        CHECK(ends == 1 && endAt != std::string::npos && armAt != std::string::npos && endAt > armAt,
              "one 'disarm (END)', after the 0x48005D52 arm (%zu)", ends);
        size_t nots = 0;
        for (size_t at = err.find("d52 not armed"); at != std::string::npos; at = err.find("d52 not armed", at + 1))
            ++nots;
        CHECK(nots == 2 && !contains(err, "PLX page absent") && !contains(err, "fence=failed") &&
                  !contains(err, "superseded"),
              "two loud lines, no absent disarm, no failed fence, nothing superseded");
    }

    ::unlink(mem);
    if (g_fails) {
        std::printf("test_d52_log: %d FAIL of %d checks\n", g_fails, g_checks);
        return 1;
    }
    std::printf("test_d52_log: OK (%d checks)\n", g_checks);
    return 0;
}
