#pragma once
// RGB24 paint for the Library Browse shell. No motion. Focus = solid bar / 2 px amber ring.

#include "libmisterplex/browse_shell_cache.hpp"
#include "libmisterplex/browse_shell_layout.hpp"
#include "libmisterplex/browse_shell_model.hpp"
#include "libmisterplex/plexui/plexui_font.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace misterplex {

struct BrowsePaintThumbs {
    BrowseRamCache* ram = nullptr;
    std::string_view serverId;
};

struct BrowseRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// Damage from the last copyFrame. full=true: dest/OSD must take the whole page.
struct FrameDamage {
    std::array<BrowseRect, 32> rects{};
    int count = 0;
    bool full = true;
};

void paintBrowseShell(const BrowseRenderModel& m, const BrowseShellLayout& L,
                      const plexui::FontSet& font, std::uint8_t* rgb, int w, int h,
                      const BrowsePaintThumbs* thumbs = nullptr, const BrowseRect* dirty = nullptr);

// Seed the mockup-matching demo catalog (host shots).
void seedBrowseDemoModel(BrowseRenderModel& m, BrowseShellScreen screen, const BrowseShellLayout& L);

} // namespace misterplex
