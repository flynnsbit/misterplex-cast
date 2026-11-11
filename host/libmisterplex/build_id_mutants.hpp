#pragma once
// Test-only mutation seams for the BUILD_ID decision points (see tests/unit/test_combined_240p_gate.cpp). Without a
// -DMUT_ID_* flag both functions are constexpr false and the compiler removes the calls: no ship-code change.
//   MUT_ID_NO_240P      0x0B2403A7 is refused everywhere (the daemon on cast-dev #51/#53 before dual-build-id)
//   MUT_ID_NO_BFCE8C5   0x0BFCE8C5 is refused
//   MUT_ID_NO_59C11A    0x0B59C11A is refused
//   MUT_ID_ACCEPT_OLD   the superseded 0x0B2402F3 and 0x0B240F9E are accepted
#include <cstdint>

namespace misterplex {
inline constexpr bool buildIdMutRejects(uint32_t id) {
#if defined(MUT_ID_NO_240P)
    if (id == 0x0B2403A7u) return true;
#endif
#if defined(MUT_ID_NO_BFCE8C5)
    if (id == 0x0BFCE8C5u) return true;
#endif
#if defined(MUT_ID_NO_59C11A)
    if (id == 0x0B59C11Au) return true;
#endif
    (void)id;
    return false;
}
inline constexpr bool buildIdMutAccepts(uint32_t id) {
#if defined(MUT_ID_ACCEPT_OLD)
    if (id == 0x0B2402F3u || id == 0x0B240F9Eu) return true;
#endif
    (void)id;
    return false;
}
} // namespace misterplex
