#pragma once
// Goal A idle contract (AGENTS.md): glass names daemon-down vs incorrect pairing
// with both BUILD_IDs. Pure helpers; CoreGate owns when they apply.
#include "core_id_select.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

namespace misterplex {

inline constexpr char kIdleDaemonDownText[] = "daemon not running";

inline std::string formatDaemonDownError() { return kIdleDaemonDownText; }

inline std::string formatPairingError(uint32_t daemonId, uint32_t coreId) {
    char buf[72];
    std::snprintf(buf, sizeof buf, "incorrect pairing daemon=0x%08X core=0x%08X", daemonId, coreId);
    return buf;
}

inline uint32_t daemonIdForPairedRaster(const std::string& raster) {
    if (raster == "480i")
        return kSelectedCoreId480i;
    if (raster == "240p")
        return kSelectedCoreId240p;
    return kSelectedCoreId480p;
}

// paintIdle sets this around DDR / R1 writes so a Rejected Plex core can show the
// pairing error. Public FpgaSpi paths (the 41 closed-gate tests) stay refused.
inline thread_local int idleErrorPaintScopeDepth = 0;

struct IdleErrorPaintGuard {
    explicit IdleErrorPaintGuard(bool arm) : armed_(arm) {
        if (armed_)
            ++idleErrorPaintScopeDepth;
    }
    ~IdleErrorPaintGuard() {
        if (armed_)
            --idleErrorPaintScopeDepth;
    }
    IdleErrorPaintGuard(const IdleErrorPaintGuard&) = delete;
    IdleErrorPaintGuard& operator=(const IdleErrorPaintGuard&) = delete;
    bool armed_;
};

inline bool idleErrorPaintScope() { return idleErrorPaintScopeDepth > 0; }

// writeStatusHeartbeat sets this so SpiExclusive may RMW status H on a Rejected
// Plex core that has a statusBlock CAPS row. Doorbell / overlay / bitstream stay
// refused (coreGateAllows). Analog VGA pairing text is FPGA overlay pair_fail.
inline thread_local int statusHeartbeatScopeDepth = 0;

struct StatusHeartbeatGuard {
    StatusHeartbeatGuard() { ++statusHeartbeatScopeDepth; }
    ~StatusHeartbeatGuard() { --statusHeartbeatScopeDepth; }
    StatusHeartbeatGuard(const StatusHeartbeatGuard&) = delete;
    StatusHeartbeatGuard& operator=(const StatusHeartbeatGuard&) = delete;
};

inline bool statusHeartbeatScope() { return statusHeartbeatScopeDepth > 0; }

}  // namespace misterplex
