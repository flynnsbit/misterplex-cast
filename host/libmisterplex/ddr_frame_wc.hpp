#pragma once
// MPX_DDR_FRAME_WC: may the frame/doorbell DDR map use the write-combining /dev/mplex_ddr window?
//
// K1-B1 (review note 7c12b8ca): a fixed mplex_ddr kmod (the second 2 MiB WC window for the R1
// host idle frame buffer) must never move a running daemon's frame and doorbell writes from
// uncached /dev/mem to WC unasked. Before this guard, FpgaSpi::ensureDdrMap used /dev/mplex_ddr
// whenever it opened and covered the layout.
//   unset, "" or "0"  never: the frame map is /dev/mem, as on a lab MiSTer today (no kmod loaded there).
//   "1"               as before this guard: WC when /dev/mplex_ddr opens and covers the layout.
//   anything else     never, and the map line says the value was not 0 or 1.
// The map logs one line each time it is made: "ddr_frame_map path=devmem wc_guard=0 ...".

#include <cstring>

namespace misterplex {

constexpr const char* kDdrFrameWcEnv = "MPX_DDR_FRAME_WC";

enum class DdrFrameWcSetting { Off, On, Invalid };

inline DdrFrameWcSetting parseDdrFrameWc(const char* raw) {
    if (!raw || !*raw || std::strcmp(raw, "0") == 0)
        return DdrFrameWcSetting::Off;
    if (std::strcmp(raw, "1") == 0)
        return DdrFrameWcSetting::On;
    return DdrFrameWcSetting::Invalid;
}

} // namespace misterplex
