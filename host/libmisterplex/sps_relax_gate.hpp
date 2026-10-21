#pragma once

#include "fpga_build_ids.hpp"

#include <cstdint>

namespace misterplex {

// Analog v10 promote of the bb006f163 SPS relax onto analog v9 (0x487A1BD1).
// Product flag lives on FpgaBuildInfo.spsRelaxNvenc (true on 0x48C5385C, 0x48C51FF1, 0x48CFEE31).
// Never reuse 0x48FF65F2, 0x487A1BD1, or 0x48FE178F. Old 0x48FBCE88 is not this row.
constexpr uint32_t kSpsRelaxV10BuildId = 0x48C5385Cu;

inline bool spsRelaxNvencRow(uint32_t id) {
    const FpgaBuildInfo* row = recognisedFpgaBuild(id);
    return row && row->spsRelaxNvenc;
}

// G9 / VUI limits from av_inproc_decode.cpp. capRefs is the CAPS maxRefs floored
// to 1 (absent = 1). refsLimit widens to 2 only on the relax row. decBufLimit,
// NAL/VCL HRD, and pic_struct follow that flag. Active reference checks (G18/G26)
// stay on capRefs. Reorder is never relaxed.
struct SpsRelaxLimits {
    uint32_t capRefs;
    uint32_t refsLimit;
    uint32_t decBufLimit;
    bool hrdAllowed;
    bool picStructAllowed;
};

inline SpsRelaxLimits spsRelaxLimits(uint32_t buildId, uint32_t capMaxRefs) {
    const bool relax = spsRelaxNvencRow(buildId);
    const uint32_t capRefs = capMaxRefs < 1u ? 1u : capMaxRefs;
    SpsRelaxLimits limits{};
    limits.capRefs = capRefs;
    limits.refsLimit = (relax && capRefs < 2u) ? 2u : capRefs;
    limits.decBufLimit = relax ? 2u : 1u;
    limits.hrdAllowed = relax;
    limits.picStructAllowed = relax;
    return limits;
}

inline bool spsDeclaredRefsOk(const SpsRelaxLimits& limits, uint32_t refs) {
    return refs <= limits.refsLimit;
}

inline bool spsActiveRefsOk(const SpsRelaxLimits& limits, uint32_t active) {
    return active <= limits.capRefs;
}

inline bool spsDecBufOk(const SpsRelaxLimits& limits, uint32_t decBuf) {
    return decBuf <= limits.decBufLimit;
}

inline bool spsReorderOk(uint32_t reorder) { return reorder == 0u; }

inline bool spsHrdOk(const SpsRelaxLimits& limits, bool present) {
    return !present || limits.hrdAllowed;
}

inline bool spsPicStructOk(const SpsRelaxLimits& limits, bool present) {
    return !present || limits.picStructAllowed;
}

}  // namespace misterplex
