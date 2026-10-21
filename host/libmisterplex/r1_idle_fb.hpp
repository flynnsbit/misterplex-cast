#pragma once
// R1 host idle frame buffer (build kit §6c R1.1/R1.2, H264 kit 4f6177b6 house layout; RTL dev/r1-idlefb-mvpb
// ddr_frame_store.sv:78-79/210-234/2328-2335, plex_fb480.sv:34-36). Bug cleared the addresses (16:4x CDT 10/4).
//   IDLE0 0x31600000, IDLE1 0x31700000: RGB24 640x480, stride 1920 (921,600 B each), inside the 2 MiB K1 window.
//   PLXH 0x300FF138 (host -> FPGA): {token[63:48], rsvd[47:34]=0, buf_sel[33], en[32], 'PLXH' 0x504C5848[31:0]}
//   PLXO 0x300FF140 (FPGA -> host): {token_latched[63:48], rsvd[47:35], live_shown[34], idle_buf_shown[33],
//                                   idle_shown[32], 'PLXO' 0x504C584F[31:0]}, written change-only by the RTL.
// Tokens are 16-bit and never 0 (K2); PLXO is zeroed by one 64-bit store before the first arm (K2).
#include "libmisterplex/mplex_ddr_abi.hpp"

#include <cstdint>

namespace misterplex {
namespace r1 {

constexpr uint32_t kIdleW = 640;
constexpr uint32_t kIdleH = 480;
constexpr uint32_t kIdleStride = kIdleW * 3;
constexpr uint32_t kIdleBytes = kIdleStride * kIdleH; // 921,600
constexpr uint32_t kIdle0Phys = 0x31600000u;
constexpr uint32_t kIdle1Phys = 0x31700000u;
constexpr uint32_t kPlxhPhys = 0x300FF138u;
constexpr uint32_t kPlxoPhys = 0x300FF140u;
constexpr uint32_t kPlxhMagic = 0x504C5848u; // "PLXH"
constexpr uint32_t kPlxoMagic = 0x504C584Fu; // "PLXO"

static_assert(kIdleBytes == 921600u, "640x480 RGB24");
static_assert(kIdle0Phys >= kMplexIdleFbPhys && kIdle0Phys + kIdleBytes <= kMplexIdleFbPhys + kMplexIdleFbBytes,
              "IDLE0 inside the K1 window");
static_assert(kIdle1Phys >= kMplexIdleFbPhys && kIdle1Phys + kIdleBytes <= kMplexIdleFbPhys + kMplexIdleFbBytes,
              "IDLE1 inside the K1 window");
static_assert(kIdle0Phys + kIdleBytes <= kIdle1Phys, "IDLE0 and IDLE1 do not overlap");
static_assert((kPlxhPhys & 7u) == 0 && (kPlxoPhys & 7u) == 0 && kPlxoPhys == kPlxhPhys + 8u, "8-byte PLX slots");

inline constexpr uint32_t idleBufPhys(int buf) { return buf ? kIdle1Phys : kIdle0Phys; }

// The next token after `t`, 16-bit, skipping 0.
inline constexpr uint16_t nextToken(uint16_t t) { return static_cast<uint16_t>(t == 0xFFFFu ? 1u : t + 1u); }

inline constexpr uint64_t encodePlxh(uint16_t token, int bufSel, bool en) {
    return (static_cast<uint64_t>(token) << 48) | (static_cast<uint64_t>(bufSel ? 1u : 0u) << 33) |
           (static_cast<uint64_t>(en ? 1u : 0u) << 32) | kPlxhMagic;
}

struct Plxo {
    bool valid = false;
    uint16_t tokenLatched = 0;
    bool liveShown = false;
    int idleBufShown = 0;
    bool idleShown = false;
};

inline constexpr Plxo decodePlxo(uint64_t w) {
    Plxo o{};
    if (static_cast<uint32_t>(w) != kPlxoMagic)
        return o;
    o.valid = true;
    o.tokenLatched = static_cast<uint16_t>(w >> 48);
    o.liveShown = ((w >> 34) & 1u) != 0;
    o.idleBufShown = static_cast<int>((w >> 33) & 1u);
    o.idleShown = ((w >> 32) & 1u) != 0;
    return o;
}

static_assert(encodePlxh(0x1234, 1, true) == ((0x1234ull << 48) | (1ull << 33) | (1ull << 32) | 0x504C5848ull),
              "PLXH layout");
static_assert(encodePlxh(0x0001, 0, false) == ((0x0001ull << 48) | 0x504C5848ull), "PLXH buf_sel/en clear");
static_assert(decodePlxo((0xBEEFull << 48) | (1ull << 34) | (1ull << 33) | (1ull << 32) | 0x504C584Full).tokenLatched ==
                  0xBEEF,
              "PLXO token");
static_assert(nextToken(0xFFFF) == 1 && nextToken(0) == 1 && nextToken(7) == 8, "16-bit nonzero tokens");

} // namespace r1
} // namespace misterplex
