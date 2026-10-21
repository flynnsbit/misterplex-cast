// MPX_DDR_FRAME_WC guard (K1-B1, review note 7c12b8ca): the real arm/misterplexd/fpga_spi.cpp in
// a host build with an unmanaged gate (MPX_CORE_GATE_START_OPEN), MPX_DEVMEM_PATH a sparse fake
// memory file and MPX_MPLEX_DDR_PATH a fake 8 MiB WC window file that EXISTS, as /dev/mplex_ddr
// would once a fixed kmod is loaded. The frame map (frames and the doorbell) must stay on /dev/mem
// unless MPX_DDR_FRAME_WC=1, and its ddr_frame_map line must say which path it took.
#include "fpga_spi.hpp"
#include "libmisterplex/ddr_frame_wc.hpp"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#ifndef MPX_MPLEX_DDR_PATH
#error "build with -DMPX_MPLEX_DDR_PATH=\"<fake WC window file>\""
#endif

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

bool makeSparse(const char* path, off_t size) {
    const int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    const bool ok = ::ftruncate(fd, size) == 0;
    ::close(fd);
    return ok;
}

// What fn writes to stderr (the daemon's log), through a file next to the fake memory.
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

struct Case {
    const char* env;      // MPX_DDR_FRAME_WC, nullptr = unset
    bool windowPresent;   // the fake /dev/mplex_ddr exists
    bool wantWc;          // the frame map is the WC window
    const char* wantLine; // on the ddr_frame_map line
    const char* wantNote; // also on it; nullptr = no note at all
};
} // namespace

int main() {
    using misterplex::DdrFrameWcSetting;
    using misterplex::parseDdrFrameWc;
    CHECK(parseDdrFrameWc(nullptr) == DdrFrameWcSetting::Off, "unset");
    CHECK(parseDdrFrameWc("") == DdrFrameWcSetting::Off, "empty");
    CHECK(parseDdrFrameWc("0") == DdrFrameWcSetting::Off, "0");
    CHECK(parseDdrFrameWc("1") == DdrFrameWcSetting::On, "1");
    for (const char* v : {"yes", "true", "01", "1 ", " 1", "2", "on"})
        CHECK(parseDdrFrameWc(v) == DdrFrameWcSetting::Invalid, "'%s' is invalid", v);

    if (!makeSparse(MPX_DEVMEM_PATH, 0x40000000) || !makeSparse(MPX_MPLEX_DDR_PATH, 0x800000)) {
        std::printf("test_ddr_frame_wc: cannot create the fake files\n");
        return 1;
    }
    const Case cases[] = {
        // a lab MiSTer after a fixed kmod is loaded, prod env unchanged: no WC, and the line says why.
        {nullptr, true, false, "ddr_frame_map path=devmem wc_guard=0",
         MPX_MPLEX_DDR_PATH " present, not used: MPX_DDR_FRAME_WC=1 allows WC"},
        {"0", true, false, "ddr_frame_map path=devmem wc_guard=0", " present, not used"},
        {"yes", true, false, "ddr_frame_map path=devmem wc_guard=0", "neither 0 nor 1: treated as 0"},
        // Opted in: WC, as before the guard.
        {"1", true, true, "ddr_frame_map path=mplex_ddr wc_guard=1", nullptr},
        // Opted in, no kmod: /dev/mem.
        {"1", false, false, "ddr_frame_map path=devmem wc_guard=1", nullptr},
        // a lab MiSTer today (no kmod), prod env: /dev/mem, no note.
        {nullptr, false, false, "ddr_frame_map path=devmem wc_guard=0", nullptr},
    };
    for (const Case& c : cases) {
        if (c.env)
            ::setenv(misterplex::kDdrFrameWcEnv, c.env, 1);
        else
            ::unsetenv(misterplex::kDdrFrameWcEnv);
        if (c.windowPresent)
            (void)makeSparse(MPX_MPLEX_DDR_PATH, 0x800000);
        else
            ::unlink(MPX_MPLEX_DDR_PATH);
        bool wc = false;
        const std::string err = captureStderr([&] {
            misterplex::FpgaSpi f;
            misterplex::PlaybackCommand cmd;
            (void)f.readInputMailbox(cmd); // maps the frame window (ensureDdrMap)
            wc = f.lastDdrMapViaMplex();
        });
        const char* label = c.env ? c.env : "unset";
        CHECK(wc == c.wantWc, "MPX_DDR_FRAME_WC=%s window=%d: WC map %d, want %d", label,
              c.windowPresent, wc, c.wantWc);
        const size_t at = err.find(c.wantLine);
        CHECK(at != std::string::npos, "MPX_DDR_FRAME_WC=%s window=%d: no '%s' in: %s", label,
              c.windowPresent, c.wantLine, err.c_str());
        const std::string line = at == std::string::npos ? std::string()
                                                         : err.substr(at, err.find('\n', at) - at);
        if (c.wantNote)
            CHECK(line.find(c.wantNote) != std::string::npos, "note '%s' missing: %s", c.wantNote,
                  line.c_str());
        else
            CHECK(line.find('(') == std::string::npos, "unexpected note: %s", line.c_str());
        CHECK(err.find("ddr_frame_map", at + 1) == std::string::npos, "one map line per map");
    }
    ::unlink(MPX_DEVMEM_PATH);
    ::unlink(MPX_MPLEX_DDR_PATH);
    if (g_fails) {
        std::printf("test_ddr_frame_wc: %d FAIL of %d checks\n", g_fails, g_checks);
        return 1;
    }
    std::printf("test_ddr_frame_wc: OK (%d checks; unset/0/invalid stay on /dev/mem with the "
                "window present, 1 maps it)\n",
                g_checks);
    return 0;
}
