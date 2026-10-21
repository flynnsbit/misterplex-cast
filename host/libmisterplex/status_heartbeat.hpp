// Status block heartbeat byte H (OSD-MENU-SPEC-1002 section 9; byte choice:
// the status-byte-H lab note, release 09:40).
//
// H is status byte 6, bits [55:48]. Today it is the telemetry byte
// first_mb_type, tied to 0 on every Plex core (bounded slice parser) and only
// parsed into CoreStatus.first_mb_type / wr_count_lo, which nothing reads.
//   H[0]   [48]     daemon  heartbeat toggle, flipped every 2 s
//   H[2:1] [50:49]  daemon  Plex server: 0 = -, 1 = Unreachable, 2 = Connected,
//                           3 = Pairing (incorrect pairing idle; analog overlay)
//   H[3]   [51]     daemon  Player: 0 = Idle, 1 = Casting
//   H[6:4] [54:52]  daemon  Source fps: 0 = -, 1 = 23.976, 2 = 24, 3 = 25,
//                           4 = 29.97, 5 = 30, 6 = 50, 7 = 59.94
//   H[7]   [55]     core    Daemon Running; never written by the daemon (a full-word
//                           UIO_SET_STATUS2 write copies it from the live echo)
// The daemon writes H only when the negotiated build row has statusBlock set
// (fpga_build_ids.hpp); no row has it today. No title: MiSTer OSD text is the
// static CONF_STR, so the title stays in the daemon log.
#pragma once

#include <cstdint>

namespace misterplex {
namespace status_heartbeat {

constexpr int kByte = 6;                // status_in / status byte 6 = bits [55:48]
constexpr int kFirstBit = kByte * 8;    // 48
constexpr uint8_t kToggle = 0x01;       // H[0]
constexpr int kServerShift = 1;         // H[2:1]
constexpr uint8_t kServerMask = 0x06;
constexpr uint8_t kCasting = 0x08;      // H[3]
constexpr int kFpsShift = 4;            // H[6:4]
constexpr uint8_t kFpsMask = 0x70;
constexpr uint8_t kRunning = 0x80;      // H[7], core-driven
constexpr uint8_t kDaemonBits = 0x7F;   // H[6:0]
constexpr int kTickMs = 2000;

enum class Server : uint8_t { Unknown = 0, Unreachable = 1, Connected = 2, Pairing = 3 };

// Source fps class for H[6:4]; 0 when there is no exact match.
inline uint8_t fpsClass(int num, int den) {
    if (num <= 0 || den <= 0)
        return 0;
    struct { int n, d; uint8_t c; } const table[] = {
        {24000, 1001, 1}, {24, 1, 2}, {25, 1, 3}, {30000, 1001, 4},
        {30, 1, 5}, {50, 1, 6}, {60000, 1001, 7},
    };
    for (const auto& t : table)
        if (static_cast<int64_t>(num) * t.d == static_cast<int64_t>(den) * t.n)
            return t.c;
    return 0;
}

// H[6:0]. H[7] is the core's and is left 0 here.
inline uint8_t compose(bool toggle, Server server, bool casting, uint8_t fps_class) {
    return static_cast<uint8_t>((toggle ? kToggle : 0) |
                                ((static_cast<uint8_t>(server) << kServerShift) & kServerMask) |
                                (casting ? kCasting : 0) |
                                ((fps_class << kFpsShift) & kFpsMask));
}

// Clean stop: H[6:1] = 0 and H[0] kept, so the core's 360-field timeout runs
// from the last real heartbeat.
inline uint8_t stopValue(uint8_t last) { return static_cast<uint8_t>(last & kToggle); }

// Lab only (release 11:46, 480i glass part B): MPX_STATUS_BLOCK_FORCE=1 makes every
// negotiated build row act as statusBlock. Exactly "1"; anything else (or unset) is off.
inline bool statusBlockForceEnv(const char* v) { return v && v[0] == '1' && v[1] == '\0'; }

}  // namespace status_heartbeat
}  // namespace misterplex
