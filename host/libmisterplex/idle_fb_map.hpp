#pragma once
// K1 §2: the R1 idle-FB map, chosen once at R1 arm time and separate from FpgaSpi::ensureDdrMap (the frame map
// path is untouched; its WC use stays behind MPX_DDR_FRAME_WC, ddr_frame_wc.hpp). Only for FpgaBuildInfo.hostIdleFb.
//   1. wc:     open(/dev/mplex_ddr) -> ioctl(MPLEX_IOC_CAPS) has bit0 -> mmap(2 MiB, MAP_SHARED, pgoff 0x1600).
//   2. devmem: open(/dev/mem, O_RDWR|O_CLOEXEC [+O_SYNC as the prod frame path, K1-N1]) -> mmap(2 MiB, 0x31600000).
//   3. off:    paint nothing and never arm PLXH en=1 (the FPGA keeps today's idle).
// MPX_IDLEFB_PATH=wc|devmem|off forces one path (a forced path that fails is off). One log line at arm:
//   "r1: idlefb path=wc|devmem|off kmod_caps=0x%x reason=<...>".
// Pacing (render spec F5 + review 18:45): at most one paint per display frame on wc; on devmem the next paint
// starts no earlier than 2 display frames (PLXD frames_done/VBL) after the previous ACK.
#include "libmisterplex/mplex_ddr_abi.hpp"
#include "libmisterplex/r1_idle_fb.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MPX_MPLEX_DDR_PATH
#define MPX_MPLEX_DDR_PATH "/dev/mplex_ddr"
#endif
#ifndef MPX_DEVMEM_PATH
#define MPX_DEVMEM_PATH "/dev/mem"
#endif

namespace misterplex {

enum class IdleFbMapPath { Off, Wc, DevMem };

inline const char* idleFbMapPathName(IdleFbMapPath p) {
    return p == IdleFbMapPath::Wc ? "wc" : p == IdleFbMapPath::DevMem ? "devmem" : "off";
}

constexpr const char* kIdleFbPathEnv = "MPX_IDLEFB_PATH";

// The syscalls the map uses; tests replace them (no device, no root).
struct IdleFbSys {
    std::function<int(const char*, int)> open = [](const char* p, int f) { return ::open(p, f); };
    std::function<int(int, unsigned long, void*)> ioctl = [](int fd, unsigned long r, void* a) {
        return ::ioctl(fd, r, a);
    };
    std::function<void*(size_t, int, int, int, off_t)> mmap = [](size_t n, int prot, int fl, int fd, off_t off) {
        return ::mmap(nullptr, n, prot, fl, fd, off);
    };
    std::function<int(void*, size_t)> munmap = [](void* p, size_t n) { return ::munmap(p, n); };
    std::function<int(int)> close = [](int fd) { return ::close(fd); };
};

class IdleFbMap {
public:
    explicit IdleFbMap(IdleFbSys sys = IdleFbSys{}) : sys_(std::move(sys)) {}
    ~IdleFbMap() { release(); }
    IdleFbMap(const IdleFbMap&) = delete;
    IdleFbMap& operator=(const IdleFbMap&) = delete;

    // Chooses and maps the path. devmemSync mirrors FpgaSpi::ddrMemSync_ (the prod frame path's O_SYNC).
    IdleFbMapPath arm(bool devmemSync) {
        release();
        caps_ = 0;
        reason_.clear();
        bool tryWc = true, tryDevmem = true;
        forced_ = std::getenv(kIdleFbPathEnv) ? std::getenv(kIdleFbPathEnv) : "";
        if (forced_ == "wc") {
            tryDevmem = false;
        } else if (forced_ == "devmem") {
            tryWc = false;
        } else if (forced_ == "off") {
            tryWc = tryDevmem = false;
            reason_ = "forced off";
        } else if (!forced_.empty()) {
            reason_ = "MPX_IDLEFB_PATH='" + forced_ + "' is not wc|devmem|off: ignored; ";
            forced_.clear();
        }
        if (tryWc && armWc())
            path_ = IdleFbMapPath::Wc;
#ifndef MUT_IDLEFB_SKIP_DEVMEM
        else if (tryDevmem && armDevmem(devmemSync))
            path_ = IdleFbMapPath::DevMem;
#endif
        else
            path_ = IdleFbMapPath::Off;
        (void)devmemSync;
        return path_;
    }

    void release() {
        if (base_)
            sys_.munmap(base_, kMplexIdleFbBytes);
        base_ = nullptr;
        if (fd_ >= 0)
            sys_.close(fd_);
        fd_ = -1;
        path_ = IdleFbMapPath::Off;
    }

    IdleFbMapPath path() const { return path_; }
    uint32_t kmodCaps() const { return caps_; }
    const std::string& reason() const { return reason_; }
    // Painting is allowed only on a mapped path; off must never arm PLXH en=1.
    bool paintAllowed() const { return path_ != IdleFbMapPath::Off && base_ != nullptr; }
    // Minimum display frames from the previous paint's ACK to the next paint start (F5, review 18:45).
    int minFramesAfterAck() const { return path_ == IdleFbMapPath::DevMem ? 2 : 1; }
    uint8_t* buffer(int idx) const {
        if (!base_ || (idx != 0 && idx != 1))
            return nullptr;
        return base_ + (r1::idleBufPhys(idx) - kMplexIdleFbPhys);
    }

    std::string logLine() const {
        char b[256];
        std::snprintf(b, sizeof b, "r1: idlefb path=%s kmod_caps=0x%x reason=%s%s", idleFbMapPathName(path_), caps_,
                      reason_.empty() ? "-" : reason_.c_str(),
                      forced_.empty() ? "" : (" (" + std::string(kIdleFbPathEnv) + "=" + forced_ + ")").c_str());
        return b;
    }

private:
    void note(const std::string& s) { reason_ += (reason_.empty() ? "" : "; ") + s; }
    static std::string err(const char* what, int e) { return std::string(what) + " " + std::strerror(e); }

    bool armWc() {
        const int fd = sys_.open(MPX_MPLEX_DDR_PATH, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            note(err("wc: open " MPX_MPLEX_DDR_PATH ":", errno));
            return false;
        }
        uint32_t caps = 0;
        if (sys_.ioctl(fd, MPLEX_IOC_CAPS, &caps) != 0) {
            note(errno == ENOTTY ? std::string("wc: ENOTTY (kmod without MPLEX_IOC_CAPS)") : err("wc: CAPS:", errno));
            sys_.close(fd);
            return false;
        }
        caps_ = caps;
        if (!(caps & kMplexCapIdleFbWc)) {
            note("wc: CAPS bit0 clear");
            sys_.close(fd);
            return false;
        }
        void* p = sys_.mmap(kMplexIdleFbBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                            static_cast<off_t>(kMplexIdleFbPgoff) * 4096);
        if (p == MAP_FAILED) {
            note(err("wc: mmap:", errno));
            sys_.close(fd);
            return false;
        }
        fd_ = fd;
        base_ = static_cast<uint8_t*>(p);
        return true;
    }

    bool armDevmem(bool sync) {
        const int fd = sys_.open(MPX_DEVMEM_PATH, O_RDWR | O_CLOEXEC | (sync ? O_SYNC : 0));
        if (fd < 0) {
            note(err("devmem: open " MPX_DEVMEM_PATH ":", errno));
            return false;
        }
        void* p = sys_.mmap(kMplexIdleFbBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                            static_cast<off_t>(kMplexIdleFbPhys));
        if (p == MAP_FAILED) {
            note(err("devmem: mmap:", errno));
            sys_.close(fd);
            return false;
        }
        fd_ = fd;
        base_ = static_cast<uint8_t*>(p);
        return true;
    }

    IdleFbSys sys_;
    IdleFbMapPath path_ = IdleFbMapPath::Off;
    uint8_t* base_ = nullptr;
    int fd_ = -1;
    uint32_t caps_ = 0;
    std::string reason_;
    std::string forced_;
};

} // namespace misterplex
