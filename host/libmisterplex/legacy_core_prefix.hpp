#pragma once

namespace misterplex {

// Prefix8 (first 8 hex digits of the RBF md5) of the RC core the rc3 boot
// path names. FPGA_VIDEO_BUILD_ID 0x48004D40, commit 47cb9fb4, RBF 999800cf
// (fpga_build_ids.hpp). The retired prod RBF is 7cbae324 (BUILD_ID 0x4800F36A,
// 82ef65c s2 map report). A deployed misterplex.conf may still say 7cbae324;
// the daemon pins this constant instead, and tests/unit/test_legacy_core_prefix.sh
// fails if boot.sh or the conf template names a different prefix.
inline constexpr char kLegacyCorePrefix[] = "999800cf";

} // namespace misterplex
