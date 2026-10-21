#pragma once
// Lab tools that drive the FPGA directly (push_frame, set_status) run under the daemon's
// core gate (core_gate.hpp), built managed like the daemon. Before any FPGA access a tool
// verifies the loaded core once: the gate's own sequence (coreGateTickStep), so the same
// reload signals, fresh-name rules, settle, single CAPS probe and allowlist. It refuses
// otherwise. It also refuses while misterplexd runs: the probe resets the AU ring under
// the daemon's session, and two writers on the frame banks and SPI would fight anyway.
#include "fpga_spi.hpp"
#include "libmisterplex/core_gate.hpp"
#include "lock_paths.hpp"
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace misterplex {

// misterplexd holds an exclusive flock on this file while it runs (main.cpp:188-197).
inline bool misterplexdRunning(const char* lockPath = daemonLockPath()) {
    const int fd = ::open(lockPath, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return false; // no daemon has run since boot
    const bool held = ::flock(fd, LOCK_EX | LOCK_NB) != 0;
    if (!held)
        ::flock(fd, LOCK_UN);
    ::close(fd);
    return held;
}

// True once the gate is open on a verified Plex build. The gate's lines go to stderr.
inline bool coreGateVerifyForTool(FpgaSpi& spi, const char* tool, int timeoutMs = 8000) {
    CoreGate& gate = CoreGate::process();
    if (!gate.managed())
        return true; // a test build of the tool (MPX_CORE_GATE_START_OPEN)
    if (misterplexdRunning()) {
        std::fprintf(stderr,
                     "%s: refusing: misterplexd is running; stop it first (the core-gate "
                     "probe would reset its AU ring)\n",
                     tool);
        return false;
    }
    bool pending = false;
    CoreGateTickHooks h;
    h.observe = [&spi] {
        CoreObservation o;
        o.fabric = spi.fabricState();
        o.main = FpgaSpi::mainIdentity();
        o.mainStartWallNs = mainStartWallNs(o.main);
        o.bootWallNs = bootWallNs();
        o.coreName = stampCoreFile(kCoreNamePath);
        o.rbfName = stampCoreFile(kRbfNamePath);
        o.nowUs = coreGateNowUs();
        return o;
    };
    h.playing = [] { return false; };
    h.probe = [&spi] { return spi.runCoreGateProbe(500); };
    h.coreReturned = [&spi] { spi.onCoreReturned(); };
    h.finishReturn = [] {};
    h.repaint = [] {};
    h.log = [](const std::string& line) { std::fprintf(stderr, "%s\n", line.c_str()); };
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        (void)coreGateTickStep(gate, h, pending);
        if (gate.open())
            return true;
        if (gate.state() == CoreGateState::Rejected ||
            std::chrono::steady_clock::now() >= deadline)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::fprintf(stderr,
                 "%s: refusing: the loaded core is not a verified Plex build (core gate "
                 "closed)\n",
                 tool);
    return false;
}

} // namespace misterplex
