#pragma once
// R1 (hostIdleFb) adapter: the P1 browse controller's current level (BrowseR1View, browse_v2.hpp) ->
// browse::RenderModel (browse_render.hpp, render spec 2d57d01b/a910c475, look LOCKED to the rg1 mockups).
// Product defaults until lead/owner rule (release 19:08):
//   Q1  rows are drawn as the helper sends them (no rating/transcode attributes);
//   Q2  the helper's exact action labels map to the mockup casing: "RESUME FROM hh:mm" -> "Resume from hh:mm",
//       "PLAY FROM START" -> "Play from start", "PLAY" -> "Play"; PREV/NEXT rows -> "Prev"/"Next" (the renderer
//       draws "< Prev" / "Next >");
//   Q5  watched: the /a1 helper appends "  W" (two spaces, then W) to the label (mpx_menu.py progress()); fold_a1
//       collapses whitespace inside titles, so a trailing double-space W can only be that mark. Drawn dim.
// The renderer still sanitises and clips every string (F3); this only shapes the model.
#include "browse_render.hpp"
#include "browse_v2.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace misterplex {
namespace browse {

// Servers / root / list / detail from the level URL (helper keys are path-only: /menu, /menu/servers,
// /menu/item/<sg>/<rk>, ...; /cNN and /a1 may follow). A PMS fallback level is a list, its section index the root.
inline BrowseState r1StateForLevel(const BrowseR1View& v) {
    std::string path = v.levelUrl;
    const size_t scheme = path.find("://");
    if (scheme != std::string::npos) {
        const size_t slash = path.find('/', scheme + 3);
        path = slash == std::string::npos ? std::string("/") : path.substr(slash);
    }
    const size_t q = path.find('?');
    if (q != std::string::npos)
        path = path.substr(0, q);
    if (!v.fromMenu)
        return path == "/library/sections" ? BrowseState::S3Root : BrowseState::S4List;
    auto under = [&](const char* p) { return path.compare(0, std::strlen(p), p) == 0; };
    if (under("/menu/servers"))
        return BrowseState::S2Servers;
    if (under("/menu/item/"))
        return BrowseState::S5Detail;
    // The root: "/menu" alone, or followed only by its /cNN and /a1 segments.
    std::string rest = path.size() >= 5 && under("/menu") ? path.substr(5) : std::string("x");
    while (!rest.empty()) {
        if (rest.compare(0, 3, "/a1") == 0 && (rest.size() == 3 || rest[3] == '/'))
            rest = rest.substr(3);
        else if (rest.size() >= 3 && rest[0] == '/' && rest[1] == 'c' && std::isdigit(static_cast<unsigned char>(rest[2]))) {
            size_t i = 2;
            while (i < rest.size() && std::isdigit(static_cast<unsigned char>(rest[i])))
                ++i;
            rest = rest.substr(i);
        } else if (rest == "/")
            rest.clear();
        else
            break;
    }
    return rest.empty() ? BrowseState::S3Root : BrowseState::S4List;
}

// Q2: the three action labels the helper keeps exact, in the mockup's casing; anything else unchanged.
inline std::string r1ActionLabel(const std::string& title) {
    static const std::string kResume = "RESUME FROM ";
    if (title.compare(0, kResume.size(), kResume) == 0)
        return "Resume from " + title.substr(kResume.size());
    if (title == "PLAY FROM START")
        return "Play from start";
    if (title == "PLAY")
        return "Play";
    return title;
}

inline Row r1Row(const LibraryRow& r) {
    Row out;
    out.label = r.title;
    // Inert rows first, by the cursor's rule (LibraryBrowser::selectable, the same set browseRowActivatable
    // refuses first): a page row that also carries mpxKind info/offline/header is Info, never a Prev/Next
    // button the cursor cannot take (review 09:48).
    const bool inert = !LibraryBrowser::selectable(r);
    if (!inert && browseRowIsPage(r)) {
        const bool next = browsePageRowIsNext(r);
        out.kind = next ? RowKind::PageNext : RowKind::PagePrev;
        out.label = next ? "Next" : "Prev";
        return out;
    }
    if (inert)
        out.kind = RowKind::Info;
    else if (r.directory)
        out.kind = RowKind::Directory;
    else
        out.kind = RowKind::Item;
    if (r.fromMenu && r.mpxKind == "action")
        out.label = r1ActionLabel(r.title);
    // Q5: a trailing "  W" is the watched mark.
    if (out.label.size() > 3 && out.label.compare(out.label.size() - 3, 3, "  W") == 0) {
        out.watched = true;
        out.label.resize(out.label.size() - 3);
    }
    if (r.mpxPct >= 1 && r.mpxPct <= 99)
        out.pct = r.mpxPct;
    return out;
}

// The renderer's 15-row viewport keeps the cursor on rows 2..12 when it can (render spec §3): the previous top
// stays unless the cursor left that band.
inline int r1ViewportTop(int cursor, int rowCount, int prevTop) {
    const int maxTop = std::max(0, rowCount - kViewRows);
    int top = std::max(0, std::min(prevTop, maxTop));
    if (cursor < top + 2)
        top = cursor - 2;
    else if (cursor > top + 12)
        top = cursor - 12;
    return std::max(0, std::min(top, maxTop));
}

inline RenderModel r1RenderModel(const BrowseR1View& v, int prevViewportTop) {
    RenderModel m;
    m.state = r1StateForLevel(v);
    m.headerTitle = v.header;
    if (m.headerTitle.empty())
        m.headerTitle = m.state == BrowseState::S2Servers ? "Servers" : (m.state == BrowseState::S3Root ? "Plex" : "");
    m.pageIndicator = v.mpxPage;
    m.rows.reserve(v.rows.size());
    for (const auto& r : v.rows)
        m.rows.push_back(r1Row(r));
    m.cursorIndex = std::max(0, std::min(v.cursor, static_cast<int>(m.rows.size()) - 1));
    m.viewportTop = r1ViewportTop(m.cursorIndex, static_cast<int>(m.rows.size()), prevViewportTop);
    m.footerText = v.status;
    const bool quietStatus = v.status.empty() || v.status == "LOADING..." || v.status == "EMPTY";
    if (m.rows.empty() && !quietStatus) {
        m.state = BrowseState::S8Error; // nothing to show but the reason
        m.error = true;
    } else if (!quietStatus) {
        m.footerError = true;           // e.g. NOT PLAYABLE HERE under a normal list
    }
    return m;
}

} // namespace browse
} // namespace misterplex
