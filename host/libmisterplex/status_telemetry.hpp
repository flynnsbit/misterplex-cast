// Host/FPGA status telemetry ABI shared constants.
//
// The residual checksum gate depends on these byte positions: pre-3.3l-1 RTL
// placed stream_bytes immediately after residual_dc, so raw[13] was the stream
// byte counter low byte. Keep these constants in sync with Plex.sv.
#pragma once

#include <cstdint>

namespace misterplex {
namespace status_telemetry {

// Telemetry byte positions. Bytes 12 (residual_dc) and 15 (recon_dbg) are
// telemetry only on cores whose build row (fpga_build_ids.hpp) has
// byte12Echoed / byte15Echoed = 0 (layout v2, CONF_STR v,9). On v3 (v,10)
// they are the echoed OSD bytes below (kOsdPositionByte / kOsdFlagsByte).
inline constexpr int kResidualDcBitLo = 96;
inline constexpr int kResidualDcBitHi = 103;
inline constexpr int kResidualCsumBitLo = 104;
inline constexpr int kResidualCsumBitHi = 111;
inline constexpr int kReconSigBitLo = 112;
inline constexpr int kReconSigBitHi = 119;
inline constexpr int kReconDbgBitLo = 120;
inline constexpr int kReconDbgBitHi = 127;

inline constexpr int kResidualDcByte = 12;
inline constexpr int kResidualCsumByte = 13;
inline constexpr int kReconSigByte = 14;
inline constexpr int kReconDbgByte = 15;

// P3-3l2 first 4x4 reconstructed-pixel signature:
// XOR of the 16 reconstructed Y samples after inverse quant + IDCT + pred=128.
// MB0 block0 golden recon is:
//   73,72,76,76, 72,74,71,73, 76,71,32,27, 76,73,27,24
inline constexpr int kReconSigMb0Block0 = 0x3B;

// P3-3l2 silicon RCA byte (raw[15]); bits [2:1] are reserved because the
// Aspect Ratio OSD splice overlaps them. Used bits:
//   bit0 coeff_nonzero, bit3 dequant_nonzero, bit4 idct_residual_nonzero,
//   bit5 recon_differs_from_pred, bit6 lat_res_ok, bit7 waited_for_residual
inline constexpr int kReconDbgCoeffNonzero = 0x01;
inline constexpr int kReconDbgDequantNonzero = 0x08;
inline constexpr int kReconDbgIdctNonzero = 0x10;
inline constexpr int kReconDbgReconDiffers = 0x20;
inline constexpr int kReconDbgLatResOk = 0x40;
inline constexpr int kReconDbgWaitedForResidual = 0x80;
inline constexpr int kReconDbgMb0Block0 = kReconDbgCoeffNonzero | kReconDbgDequantNonzero |
                                          kReconDbgIdctNonzero | kReconDbgReconDiffers |
                                          kReconDbgLatResOk | kReconDbgWaitedForResidual;

// OSD-MENU-SPEC-1002 status layout v3 (240p/480i lab RBFs, MPX_OSD_V10,
// CONF_STR v,10; rows 2400F40B and 4801F40A in fpga_build_ids.hpp): the
// core echoes byte 12 ([103:96]: V position [99:97], H position [103:100]) and
// byte 15 ([127:120]: Bob [120], AR [122:121], field order [123], 16:9 sources
// [124], top-swap [126]) back to Main, so they carry the user's OSD settings
// (they were residual_dc and p3_recon_dbg, stats only). A daemon status write
// must carry their live value, apart from the bits it sets itself, or it
// resets the user's settings from the private shadow (e.g. the audio flush at
// every play start). Layout v2 cores never read those status bits (Main adopts
// the telemetry there anyway), so the overlay is harmless on them.
inline constexpr int kOsdPositionByte = 12;
inline constexpr int kOsdFlagsByte = 15;

// Copy the live OSD bytes into a status word before the caller applies its
// own bits (which then win).
inline void overlayLiveOsdBytes(uint8_t word[16], const uint8_t live[16]) {
    word[kOsdPositionByte] = live[kOsdPositionByte];
    word[kOsdFlagsByte] = live[kOsdFlagsByte];
}

struct OsdStatusV3 {
    int h_position = 0;           // [103:100], s4, -8..+7, + = right
    int v_position = 0;           // [99:97], s3, -4..+3, + = down (RTL clamps)
    bool bob = false;             // [120] Deinterlace: Weave/Bob (480i)
    int aspect = 0;               // [122:121]
    bool field_inverted = false;  // [123] Field order: Normal/Inverted (480i)
    bool crop_169 = false;        // [124] 16:9 sources: Letterbox/Crop to 4:3
    bool top_swap = false;        // [126] cadence top-swap (hidden, daemon-set)
};

inline OsdStatusV3 decodeOsdStatusV3(const uint8_t raw[16]) {
    OsdStatusV3 o;
    const unsigned p = raw[kOsdPositionByte], f = raw[kOsdFlagsByte];
    o.h_position = static_cast<int>((p >> 4) & 0xF);
    if (o.h_position >= 8)
        o.h_position -= 16;
    o.v_position = static_cast<int>((p >> 1) & 0x7);
    if (o.v_position >= 4)
        o.v_position -= 8;
    o.bob = f & 0x01;
    o.aspect = static_cast<int>((f >> 1) & 0x3);
    o.field_inverted = (f >> 3) & 1;
    o.crop_169 = (f >> 4) & 1;
    o.top_swap = (f >> 6) & 1;
    return o;
}

} // namespace status_telemetry
} // namespace misterplex
