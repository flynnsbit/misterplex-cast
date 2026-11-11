#pragma once

// #228: the idle-card link row label. gcastLinkLine builds the row with it and
// reportReadyHint looks for it, so the two cannot drift apart.

#include <string>

namespace misterplex {
inline constexpr const char kPlexTvLinkLabel[] = "plex.tv/link";
// The loop publishes this sentinel instead of a code after repeated token errors, and
// gcastLinkLine shows kPlexTvLinkErrorLine for it. It never reaches plex_link.txt.
// Published while linking is armed but no PIN is on screen yet (first request, or plex.tv
// unreachable), so the link row still owns the card instead of the Report row.
inline constexpr const char kPlexTvLinkPendingCode[] = "!LINKWAIT";
inline constexpr const char kPlexTvLinkPendingLine[] = "plex.tv/link: connecting, retrying";
inline bool isPlexTvLinkMarker(const std::string& c) { return !c.empty() && c[0] == '!'; }
inline constexpr const char kPlexTvLinkErrorCode[] = "!LINKERR";
inline constexpr const char kPlexTvLinkErrorLine[] = "Plex link error, retrying";
} // namespace misterplex
