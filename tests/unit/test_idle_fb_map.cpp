// K1 KM-2: the R1 idle-FB map path choice (idle_fb_map.hpp) on a fake syscall layer (no device, no root, no
// process spawned). (a)-(f) of k1-kmod-wc-window-note-1004 §6; (g) is test_ddr_frame_wc (MPX_DDR_FRAME_WC guard).
// Mutant MUT_IDLEFB_SKIP_DEVMEM (the fallback skips /dev/mem: wc -> off) must FAIL case (b).
#include "libmisterplex/idle_fb_map.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace misterplex;

namespace {
int g_fails = 0, g_checks = 0;
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

struct Fake {
    // Behaviour knobs.
    int mplexOpenErr = 0;    // errno for open(/dev/mplex_ddr), 0 = ok
    int capsErr = 0;         // errno for ioctl(CAPS), 0 = ok
    uint32_t caps = 1;
    int wcMmapErr = 0;       // errno for the pgoff 0x1600 mmap
    int devmemOpenErr = 0;
    int devmemMmapErr = 0;
    // Observations.
    std::vector<std::string> opens;
    std::vector<int> openFlags;
    std::vector<off_t> mmapOffs;
    std::vector<int> mmapFlags, mmapProts;
    std::vector<size_t> mmapLens;
    int closes = 0, munmaps = 0, liveFds = 0;
    alignas(4096) static inline unsigned char wcMem[0x200000];
    alignas(4096) static inline unsigned char devMem[0x200000];

    IdleFbSys sys() {
        IdleFbSys s;
        s.open = [this](const char* p, int f) {
            opens.push_back(p);
            openFlags.push_back(f);
            const bool mplex = std::string(p) == MPX_MPLEX_DDR_PATH;
            const int e = mplex ? mplexOpenErr : devmemOpenErr;
            if (e) {
                errno = e;
                return -1;
            }
            ++liveFds;
            return mplex ? 100 : 200;
        };
        s.ioctl = [this](int fd, unsigned long req, void* arg) {
            if (fd != 100 || req != MPLEX_IOC_CAPS) {
                errno = EINVAL;
                return -1;
            }
            if (capsErr) {
                errno = capsErr;
                return -1;
            }
            *static_cast<uint32_t*>(arg) = caps;
            return 0;
        };
        s.mmap = [this](size_t n, int prot, int fl, int fd, off_t off) -> void* {
            mmapLens.push_back(n);
            mmapProts.push_back(prot);
            mmapFlags.push_back(fl);
            mmapOffs.push_back(off);
            const int e = fd == 100 ? wcMmapErr : devmemMmapErr;
            if (e) {
                errno = e;
                return MAP_FAILED;
            }
            return fd == 100 ? static_cast<void*>(wcMem) : static_cast<void*>(devMem);
        };
        s.munmap = [this](void*, size_t) {
            ++munmaps;
            return 0;
        };
        s.close = [this](int) {
            ++closes;
            --liveFds;
            return 0;
        };
        return s;
    }
};

std::string has(const std::string& s, const char* n) { return s.find(n) != std::string::npos ? "" : s; }
} // namespace

int main() {
    ::unsetenv(kIdleFbPathEnv);
    // (a) caps bit0 + mmap OK -> wc, one 2 MiB MAP_SHARED RW mapping at pgoff 0x1600.
    {
        Fake f;
        IdleFbMap m(f.sys());
        CHECK(m.arm(true) == IdleFbMapPath::Wc, "(a) path %s", idleFbMapPathName(m.path()));
        CHECK(f.mmapOffs.size() == 1 && f.mmapOffs[0] == static_cast<off_t>(0x1600) * 4096 &&
                  f.mmapLens[0] == 0x200000 && f.mmapFlags[0] == MAP_SHARED &&
                  f.mmapProts[0] == (PROT_READ | PROT_WRITE),
              "(a) mmap args");
        CHECK(f.opens.size() == 1 && f.opens[0] == MPX_MPLEX_DDR_PATH && (f.openFlags[0] & O_ACCMODE) == O_RDWR &&
                  (f.openFlags[0] & O_CLOEXEC),
              "(a) open " MPX_MPLEX_DDR_PATH " O_RDWR|O_CLOEXEC (VM_SHARED needs a writable fd)");
        CHECK(m.buffer(0) == Fake::wcMem && m.buffer(1) == Fake::wcMem + 0x100000 && !m.buffer(2),
              "(a) IDLE0 at +0, IDLE1 at +1 MiB");
        CHECK(m.kmodCaps() == 1 && m.paintAllowed() && m.minFramesAfterAck() == 1, "(a) caps/paint/pace");
        CHECK(has(m.logLine(), "r1: idlefb path=wc kmod_caps=0x1 reason=-").empty(), "(a) log: %s", m.logLine().c_str());
        m.release();
        CHECK(f.munmaps == 1 && f.liveFds == 0 && !m.paintAllowed(), "(a) release unmaps and closes");
    }
    // (b) ENOTTY (an old kmod) -> devmem, with the prod frame path's flags (O_SYNC per ddrMemSync_).
    for (bool sync : {true, false}) {
        Fake f;
        f.capsErr = ENOTTY;
        IdleFbMap m(f.sys());
        CHECK(m.arm(sync) == IdleFbMapPath::DevMem, "(b) sync=%d path %s", sync, idleFbMapPathName(m.path()));
        CHECK(f.opens.size() == 2 && f.opens[1] == MPX_DEVMEM_PATH &&
                  ((f.openFlags[1] & O_SYNC) == O_SYNC) == sync && (f.openFlags[1] & O_ACCMODE) == O_RDWR,
              "(b) sync=%d devmem flags 0x%x", sync, f.openFlags.size() > 1 ? f.openFlags[1] : -1);
        CHECK(f.mmapOffs.size() == 1 && f.mmapOffs[0] == static_cast<off_t>(0x31600000) && f.mmapLens[0] == 0x200000,
              "(b) devmem mmap at 0x31600000");
        CHECK(m.buffer(1) == Fake::devMem + 0x100000 && m.minFramesAfterAck() == 2, "(b) IDLE1 + F5 pace 2 frames");
        CHECK(has(m.logLine(), "path=devmem kmod_caps=0x0 reason=wc: ENOTTY").empty(), "(b) log: %s",
              m.logLine().c_str());
        CHECK(f.liveFds == 1, "(b) the failed wc fd was closed (%d live)", f.liveFds);
    }
    // (c) caps OK, mmap EINVAL -> devmem; bit0 clear -> devmem; no /dev/mplex_ddr -> devmem.
    {
        Fake f;
        f.wcMmapErr = EINVAL;
        IdleFbMap m(f.sys());
        CHECK(m.arm(true) == IdleFbMapPath::DevMem && f.liveFds == 1, "(c) mmap EINVAL");
        CHECK(has(m.logLine(), "reason=wc: mmap:").empty(), "(c) log: %s", m.logLine().c_str());
    }
    {
        Fake f;
        f.caps = 0;
        IdleFbMap m(f.sys());
        CHECK(m.arm(true) == IdleFbMapPath::DevMem && has(m.logLine(), "CAPS bit0 clear").empty(), "(c) bit0 clear");
    }
    {
        Fake f;
        f.mplexOpenErr = ENOENT;
        IdleFbMap m(f.sys());
        CHECK(m.arm(true) == IdleFbMapPath::DevMem && f.mmapOffs.size() == 1, "(c) no kmod node");
    }
    // (d) /dev/mem fails too -> off: nothing mapped, painting (and PLXH en=1) not allowed.
    {
        Fake f;
        f.capsErr = ENOTTY;
        f.devmemOpenErr = EACCES;
        IdleFbMap m(f.sys());
        CHECK(m.arm(true) == IdleFbMapPath::Off && !m.paintAllowed() && !m.buffer(0) && f.liveFds == 0, "(d) off");
        CHECK(has(m.logLine(), "path=off").empty() && has(m.logLine(), "devmem: open").empty(), "(d) log: %s",
              m.logLine().c_str());
    }
    {
        Fake f;
        f.mplexOpenErr = ENOENT;
        f.devmemMmapErr = EPERM;
        IdleFbMap m(f.sys());
        CHECK(m.arm(false) == IdleFbMapPath::Off && f.liveFds == 0, "(d) devmem mmap fails -> off, fd closed");
    }
    // (e) F5: devmem paces 2 display frames after the ACK; wc 1 (see the planner's host unit row in phase 2).
    {
        Fake f;
        IdleFbMap m(f.sys());
        m.arm(true);
        CHECK(m.minFramesAfterAck() == 1, "(e) wc pace");
        f.capsErr = ENOTTY;
        m.arm(true);
        CHECK(m.path() == IdleFbMapPath::DevMem && m.minFramesAfterAck() == 2, "(e) devmem pace");
    }
    // (f) MPX_IDLEFB_PATH overrides.
    {
        Fake f;
        f.capsErr = ENOTTY;
        ::setenv(kIdleFbPathEnv, "wc", 1);
        IdleFbMap m(f.sys());
        CHECK(m.arm(true) == IdleFbMapPath::Off && f.opens.size() == 1, "(f) forced wc that fails is off, no devmem");
        CHECK(has(m.logLine(), "(MPX_IDLEFB_PATH=wc)").empty(), "(f) log: %s", m.logLine().c_str());
    }
    {
        Fake f;
        ::setenv(kIdleFbPathEnv, "devmem", 1);
        IdleFbMap m(f.sys());
        CHECK(m.arm(true) == IdleFbMapPath::DevMem && f.opens.size() == 1 && f.opens[0] == MPX_DEVMEM_PATH,
              "(f) forced devmem never opens " MPX_MPLEX_DDR_PATH);
    }
    {
        Fake f;
        ::setenv(kIdleFbPathEnv, "off", 1);
        IdleFbMap m(f.sys());
        CHECK(m.arm(true) == IdleFbMapPath::Off && f.opens.empty() && has(m.logLine(), "reason=forced off").empty(),
              "(f) forced off opens nothing: %s", m.logLine().c_str());
    }
    {
        Fake f;
        ::setenv(kIdleFbPathEnv, "yes", 1);
        IdleFbMap m(f.sys());
        CHECK(m.arm(true) == IdleFbMapPath::Wc && has(m.logLine(), "is not wc|devmem|off: ignored").empty(),
              "(f) an invalid value is ignored: %s", m.logLine().c_str());
    }
    ::unsetenv(kIdleFbPathEnv);
    // Re-arm releases the previous map first (no fd or mapping leak across arms).
    {
        Fake f;
        IdleFbMap m(f.sys());
        m.arm(true);
        m.arm(true);
        CHECK(f.munmaps == 1 && f.liveFds == 1, "re-arm released the first map (%d unmaps, %d fds)", f.munmaps, f.liveFds);
    }
    if (g_fails) {
        std::printf("test_idle_fb_map: %d FAIL of %d checks\n", g_fails, g_checks);
        return 1;
    }
    std::printf("test_idle_fb_map: OK (%d checks; KM-2 a-f)\n", g_checks);
    return 0;
}
