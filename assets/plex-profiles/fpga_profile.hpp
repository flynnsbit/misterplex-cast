#pragma once

#include <cstddef>
#include <string>
#include "libmisterplex/ddr_bitstream_ring.hpp"

namespace misterplex {

// Experimental stream contracts, not decoder/display capabilities.
struct FpgaPmsProfile {
    std::string clientProfileName;
    std::string videoResolution;
    int width = 0, height = 0;
    int fpsNum = 0, fpsDen = 0;
    int maxVideoBitrateKbps = 4000;
    int vbvBufferKbits = 1000;
    // Physical encoded-AU ceiling only. Apply negotiated AU and VCL-RBSP limits independently.
    std::size_t maxAuBytes = ddr_bitstream_ring::kMaxAccessUnitBytes;
    int maxGop = 24;
    bool allIdr = false;
    bool filteringOff = false;
    bool reserved = false;
};

inline bool selectFpgaPmsProfile(const std::string& prototype, const std::string& mode,
                                int fpsNum, int fpsDen, bool filteringOff,
                                FpgaPmsProfile& out, std::string* why = nullptr,
                                bool allowReservedExperiment = false) {
    auto fail = [&](const char* message) {
        out = {};
        if (why) *why = message;
        return false;
    };
    if (prototype != "idr" && prototype != "ip")
        return fail("prototype must be idr or ip");
    if (!((fpsNum == 24 && fpsDen == 1) || (fpsNum == 24000 && fpsDen == 1001)))
        return fail("content rate must be 24/1 or 24000/1001; output refresh is separate");
    FpgaPmsProfile p;
    if (mode == "240p") {
        p.width = 320; p.height = 240;
    } else if (mode == "480p" || mode == "480i") {
        // Coded product rectangle matches PLEX_DECODE_480P FRAME_W/H (39×30 MB).
        // Do not advertise 640x480 into a 624-wide DPB.
        p.width = 624; p.height = 480; p.reserved = true;
    } else if (mode == "720p") {
        p.width = 1280; p.height = 720; p.reserved = true;
    } else {
        return fail("unknown decoded-picture mode");
    }
    if (p.reserved && !allowReservedExperiment)
        return fail("480/720 decoded caps are RESERVED, not advertised or accepted");
    p.allIdr = prototype == "idr";
    p.maxGop = p.allIdr ? 1 : 24;
    p.filteringOff = filteringOff;
    p.fpsNum = fpsNum; p.fpsDen = fpsDen;
    p.videoResolution = std::to_string(p.width) + "x" + std::to_string(p.height);
    const std::string tier = p.height == 480 ? "480" : mode;
    p.clientProfileName = "MiSTerPlex-FPGA-" + std::string(p.allIdr ? "IDR-" : "IP-") +
        tier + "-" + (fpsDen == 1 ? "24" : "23976") +
        (filteringOff ? "-filter-off" : "-filter-on");
    out = p;
    if (why) why->clear();
    return true;
}

} // namespace misterplex
