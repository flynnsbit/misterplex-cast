#pragma once
// Official TV chrome timing from spelunkr.tv JS (index-zkq2Puf2.js, 2026-11-07).
// DESIGN.md 3s+0.5s fade is a proposal; official default is 5s then 0.4s fade.
// Shawn 2026-11-07: MiSTer OSD channel banner auto-hides at 2 s.

#include <cstdint>

namespace misterplex::spelunkr {

inline constexpr int64_t kOfficialTvBarMs = 5000;     // L2; ?tvbar= 200..60000
inline constexpr int64_t kOfficialTvBarFadeMs = 400;  // CSS --bar-ms: .4s hide
inline constexpr int64_t kOfficialTvBarShowFadeMs = 200; // CSS --bar-ms: .2s show
inline constexpr int64_t kOfficialTvPreviewMs = 3000; // D2; ?tvpreview= 50..10000
inline constexpr int64_t kOfficialChannelRollMs = 300; // ak
inline constexpr int64_t kDesignProposalShowMs = 3000; // DESIGN.md only
inline constexpr int64_t kDesignProposalFadeMs = 500;  // DESIGN.md only
// OSD overlay clock (steady). Not the official site 5 s tvbar.
inline constexpr int64_t kSpelunkrBannerMs = 2000;
inline constexpr int64_t kSpelunkrBannerFadeMs = kOfficialTvBarFadeMs;

// Owenb OSD toast on the PlexOB overlay plane (ovl_plane), not the official site timer.
inline constexpr int64_t kOwenbToastMs = 3000; // owenb_osd_renderer.hpp UiModel::kToastMs

// Banner and guide overlays for Spelunkr use owenb_osd on the same MisterPlex core.

} // namespace misterplex::spelunkr
