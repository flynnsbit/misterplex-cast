#pragma once
// OWENB-P1B 0010: the daemon's library browser as the OB OSD's library page. The browser itself is
// unchanged: BrowseController (browse_controller.hpp:202 r1View, MPX_BROWSE_V2) or the legacy PMS
// LibraryBrowser (library_browser.hpp:215) fetch, parse and move the cursor through the daemon's own
// PMS / resolve code; this turns the level on show into a window of rows for owenb_osd_draw's
// paintLibrary. The navigation reaching the browser is the OB input (owenb_input.hpp -> obRoute ->
// MediaPlayer::dispatchPlaybackInput), so the page is the R1 browser's, drawn on the overlay instead
// of the 0x31600000 idle framebuffer (which an OB image never touches).

#include "library_browser.hpp"
#include "owenb_osd_draw.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace misterplex::owenb {

constexpr int kObLibraryWindow = 64;   // rows handed to the OSD: far more than any layout shows

inline owenb_osd::LibraryPage obLibraryPage(bool visible, const std::vector<LibraryRow>& rows, int cursor,
                                            const std::string& status, const std::string& header,
                                            int window = kObLibraryWindow) {
    owenb_osd::LibraryPage p;
    p.open = visible;
    if (!visible)
        return p;
    p.header = header;
    p.status = status;
    const int n = int(rows.size());
    p.total = n;
    if (n == 0)
        return p;
    cursor = std::clamp(cursor, 0, n - 1);
#ifdef MUT_LIBRARY_WINDOW_FROM_TOP
    const int first = 0;   // mutant: the window never follows the cursor past row 64
#else
    const int first = std::clamp(cursor - window / 2, 0, std::max(0, n - window));
#endif
    const int last = std::min(n, first + window);
    for (int i = first; i < last; ++i) {
        const LibraryRow& r = rows[size_t(i)];
        owenb_osd::LibraryRow o;
        o.text = r.title.empty() ? r.key : r.title;
        o.dir = r.directory;
#ifdef MUT_LIBRARY_INFO_SELECTABLE
        o.selectable = true;   // mutant: the helper's info / header rows look like entries
#else
        o.selectable = LibraryBrowser::selectable(r);
#endif
        p.rows.push_back(std::move(o));
    }
    p.offset = first;
    p.cursor = cursor - first < int(p.rows.size()) ? cursor - first : -1;
    return p;
}

inline owenb_osd::LibraryPage obLibraryPage(const LibraryBrowser& br, const std::string& header,
                                            int window = kObLibraryWindow) {
    static const std::vector<LibraryRow> none;
    return obLibraryPage(br.visible, br.stack.empty() ? none : br.stack.back(), br.cursor(), br.status, header,
                         window);
}

} // namespace misterplex::owenb
