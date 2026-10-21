#pragma once
// Home PMS library list for the idle framebuffer. Daemon-only, zero ALMs.
//
// MPX_BROWSE_LIBLIST is exactly "1" or the daemon does not call this. Unset, empty,
// "0", and "true" stay off, so the chevron and the play path are unchanged.
//
// On open: GET /library/sections, then each section's /all. The idle FB is RGB24
// 640x480, stride 1920 (r1_idle_fb.hpp). Titles are painted in the left 624 columns,
// the 480p picture width. Columns 624..639 stay the list background.
//
// Up/down/A/B are the DDR mailbox commands the input poll already reads (the path
// inputguard logs as "input cmd="). Prod cores emit PlayPause/Stop/SkipForward/
// SkipBack; wide cores also emit Up/Down/Back. A on an item is the companion
// playMedia key. B hides the list so the idle chevron paints again.

#include "input_mailbox.hpp"
#include "library_browser.hpp"
#include "../../arm/misterplexd/font_8x16.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace misterplex {
namespace librarylist {

constexpr const char* kBrowseLibListEnv = "MPX_BROWSE_LIBLIST";
constexpr int kLibraryListW = 624;
constexpr int kLibraryListH = 480;
constexpr int kLibraryListStride = kLibraryListW * 3;
constexpr int kLibraryListViewRows = 16;
constexpr int kLibraryListRowH = 24;
constexpr int kLibraryListHeaderY = 8;
constexpr int kLibraryListRuleY = 40;
constexpr int kLibraryListY0 = 48;
constexpr int kLibraryListFooterY = 456;
constexpr int kLibraryListTextX = 32;
constexpr int kLibraryListMarkX = 16;
constexpr int kGlyphW = 8;
constexpr int kGlyphH = 16;
constexpr int kTitleColumns = 70;
constexpr int kLibraryListMaxSections = 32;
constexpr int kLibraryListMaxItemsPerSection = 200;

inline constexpr std::uint8_t kBg[3] = {0x12, 0x16, 0x1C};
inline constexpr std::uint8_t kText[3] = {0xE8, 0xE8, 0xE8};
inline constexpr std::uint8_t kDim[3] = {0x9A, 0xA0, 0xA6};
inline constexpr std::uint8_t kAmber[3] = {0xE5, 0xA0, 0x0D};
inline constexpr std::uint8_t kInk[3] = {0x10, 0x14, 0x18};
inline constexpr std::uint8_t kRule[3] = {0x2A, 0x30, 0x38};

enum class ListKind { Section, Item };

struct ListRow {
    ListKind kind = ListKind::Item;
    std::string title;
    std::string key;
    std::string sectionKey;
};

struct LibraryList {
    std::string header = "Library";
    std::string status;
    std::vector<ListRow> rows;
    int cursor = 0;
};

struct SectionFetch {
    std::string key;
    std::string title;
    std::string path;
};

struct SectionBody {
    std::string key;
    std::string xml;
};

struct HomeLibraryFetch {
    LibraryList list;
    std::vector<std::string> paths;
    bool sectionsOk = false;
};

using PathGetFn = std::function<std::string(const std::string& path)>;

inline bool browseLibListEnabled(const char* raw) {
    return raw && std::strcmp(raw, "1") == 0;
}

inline std::string listSanitize(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (c < 0x20 || c > 0x7E)
            out.push_back('?');
        else
            out.push_back(static_cast<char>(c));
    }
    return out;
}

inline std::string listClip(const std::string& in, int columns) {
    if (columns <= 0)
        return {};
    if (static_cast<int>(in.size()) <= columns)
        return in;
    if (columns <= 3)
        return in.substr(0, static_cast<std::size_t>(columns));
    return in.substr(0, static_cast<std::size_t>(columns - 3)) + "...";
}

// PMS section keys are "1" or "/library/sections/1". Items are always the /all child.
inline std::string sectionItemsPath(const LibraryRow& row) {
    std::string path = libraryListPath(row);
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, "/all") == 0)
        return path;
    if (!path.empty() && path.back() == '/')
        path.pop_back();
    return path + "/all";
}

inline std::vector<SectionFetch> sectionFetches(const std::string& sectionsXml) {
    std::vector<SectionFetch> out;
    for (const LibraryRow& row : parseLibraryXml(sectionsXml)) {
        if (!row.directory || row.key.empty())
            continue;
        if (static_cast<int>(out.size()) >= kLibraryListMaxSections)
            break;
        SectionFetch s;
        s.key = row.key;
        s.title = row.title.empty() ? row.key : row.title;
        s.path = sectionItemsPath(row);
        out.push_back(std::move(s));
    }
    return out;
}

inline LibraryList buildLibraryList(const std::string& sectionsXml,
                                   const std::vector<SectionBody>& bodies, bool sectionsOk) {
    LibraryList list;
    if (!sectionsOk) {
        list.status = "FETCH FAILED";
        return list;
    }
    const std::vector<SectionFetch> secs = sectionFetches(sectionsXml);
    bool truncated = false;
    int dirCount = 0;
    for (const LibraryRow& row : parseLibraryXml(sectionsXml)) {
        if (row.directory && !row.key.empty())
            ++dirCount;
    }
    if (dirCount > kLibraryListMaxSections)
        truncated = true;
    for (const SectionFetch& sec : secs) {
        ListRow header;
        header.kind = ListKind::Section;
        header.title = sec.title;
        header.key = sec.key;
        header.sectionKey = sec.key;
        list.rows.push_back(std::move(header));
        const std::string* xml = nullptr;
        for (const SectionBody& body : bodies) {
            if (body.key == sec.key) {
                xml = &body.xml;
                break;
            }
        }
        if (!xml || xml->empty())
            continue;
        int n = 0;
        for (const LibraryRow& row : parseLibraryXml(*xml)) {
            if (row.title.empty() && row.ratingKey.empty() && row.key.empty())
                continue;
            if (n >= kLibraryListMaxItemsPerSection) {
                truncated = true;
                break;
            }
            ListRow item;
            item.kind = ListKind::Item;
            item.title = !row.title.empty() ? row.title : (!row.ratingKey.empty() ? row.ratingKey : row.key);
            item.key = !row.ratingKey.empty() ? row.ratingKey : row.key;
            item.sectionKey = sec.key;
            list.rows.push_back(std::move(item));
            ++n;
        }
    }
    if (list.rows.empty())
        list.status = "EMPTY";
    else if (truncated)
        list.status = "TRUNCATED";
    for (int i = 0; i < static_cast<int>(list.rows.size()); ++i) {
        if (list.rows[static_cast<std::size_t>(i)].kind == ListKind::Item) {
            list.cursor = i;
            break;
        }
    }
    return list;
}

inline HomeLibraryFetch fetchHomeLibrary(const PathGetFn& get) {
    HomeLibraryFetch out;
    if (!get) {
        out.list.status = "NO PLEX SERVER";
        return out;
    }
    out.paths.push_back("/library/sections");
    const std::string sections = get("/library/sections");
    if (sections.empty()) {
        out.list.status = "FETCH FAILED";
        return out;
    }
    out.sectionsOk = true;
    const std::vector<SectionFetch> secs = sectionFetches(sections);
    std::vector<SectionBody> bodies;
    bodies.reserve(secs.size());
    for (const SectionFetch& sec : secs) {
        out.paths.push_back(sec.path);
        SectionBody body;
        body.key = sec.key;
        body.xml = get(sec.path);
        bodies.push_back(std::move(body));
    }
    out.list = buildLibraryList(sections, bodies, true);
    return out;
}

inline int libraryListViewportTop(const LibraryList& list) {
    const int n = static_cast<int>(list.rows.size());
    const int maxTop = std::max(0, n - kLibraryListViewRows);
    int top = list.cursor - 2;
    if (top < 0)
        top = 0;
    if (top > maxTop)
        top = maxTop;
    if (list.cursor < top)
        top = list.cursor;
    if (list.cursor >= top + kLibraryListViewRows)
        top = list.cursor - kLibraryListViewRows + 1;
    if (top < 0)
        top = 0;
    if (top > maxTop)
        top = maxTop;
    return top;
}

inline void moveLibraryList(LibraryList& list, int delta) {
    if (list.rows.empty() || delta == 0)
        return;
    const int n = static_cast<int>(list.rows.size());
    int c = list.cursor + delta;
    if (c < 0)
        c = 0;
    if (c >= n)
        c = n - 1;
    list.cursor = c;
}

inline const ListRow* libraryListCurrent(const LibraryList& list) {
    if (list.cursor < 0 || list.cursor >= static_cast<int>(list.rows.size()))
        return nullptr;
    return &list.rows[static_cast<std::size_t>(list.cursor)];
}

// What one mailbox command does to the list. The daemon applies it; the input
// thread does not fetch or call play itself.
enum class BrowseListEffect : uint8_t {
    Ignore = 0,    // playback owns this press
    Open,          // fetch and show the list
    Move,          // cursor moved
    Play,          // A on an item: same key companion playMedia uses
    IdleChevron,   // B: hide the list; the idle painter draws the chevron
    Note,          // status line only
    Hold,          // consumed, list unchanged (A on a section header)
};

struct BrowseListStep {
    BrowseListEffect effect = BrowseListEffect::Ignore;
    std::string playKey;       // item ratingKey handed to the play hook
    std::string playMediaKey;  // /library/metadata/<ratingKey>
};

// Companion playMedia key for a library item. The daemon's library play hook
// builds this same path before stagePlay + doPlay.
// A numeric rating key becomes /library/metadata/N. A key that is already that
// path is the play path; prefixing it again asks PMS for a row that does not exist.
inline std::string browseListPlayMediaKey(const std::string& ratingKey) {
    const std::string prefix = "/library/metadata/";
    if (ratingKey.size() >= prefix.size() && ratingKey.compare(0, prefix.size(), prefix) == 0)
        return ratingKey;
    return prefix + ratingKey;
}

inline std::string browseListPlayRatingKey(const std::string& ratingKey) {
    const std::string path = browseListPlayMediaKey(ratingKey);
    const std::string prefix = "/library/metadata/";
    if (path.compare(0, prefix.size(), prefix) != 0)
        return ratingKey;
    std::size_t end = prefix.size();
    while (end < path.size() && path[end] >= '0' && path[end] <= '9')
        ++end;
    if (end == prefix.size())
        return ratingKey;
    return path.substr(prefix.size(), end - prefix.size());
}

// Whether libraryHandleInputList consumes this mailbox command. A live session
// keeps Skip/PlayPause/Stop. The daemon handles Browse before this helper so
// play still stops and opens the list.
inline bool libraryListOwnsInput(bool visible, bool playing, PlaybackCommand command) {
    if (playing)
        return false;
    if (visible)
        return true;
    return command == PlaybackCommand::Stop;
}

inline BrowseListStep stepBrowseListInput(LibraryList& list, bool& visible, bool playing,
                                          PlaybackCommand command) {
    BrowseListStep step;
    if (command == PlaybackCommand::Browse) {
        step.effect = BrowseListEffect::Open;
        return step;
    }
    if (!visible) {
        if (!playing && command == PlaybackCommand::Stop)
            step.effect = BrowseListEffect::Open;
        return step;
    }

    int delta = 0;
    switch (command) {
    case PlaybackCommand::Up:
    case PlaybackCommand::SkipBack:
        delta = -1;
        break;
    case PlaybackCommand::Down:
    case PlaybackCommand::SkipForward:
        delta = 1;
        break;
    case PlaybackCommand::PageUp:
        delta = -kLibraryListViewRows;
        break;
    case PlaybackCommand::PageDown:
        delta = kLibraryListViewRows;
        break;
    default:
        break;
    }
    if (delta != 0) {
        moveLibraryList(list, delta);
        step.effect = BrowseListEffect::Move;
        return step;
    }

    if (command == PlaybackCommand::PlayPause) {
        const ListRow* cur = libraryListCurrent(list);
        if (cur && cur->kind == ListKind::Item && !cur->key.empty()) {
            step.effect = BrowseListEffect::Play;
            step.playKey = cur->key;
            step.playMediaKey = browseListPlayMediaKey(cur->key);
            visible = false;
        } else {
            step.effect = BrowseListEffect::Hold;
        }
        return step;
    }

    if (command == PlaybackCommand::Stop || command == PlaybackCommand::Back) {
        visible = false;
        step.effect = BrowseListEffect::IdleChevron;
        return step;
    }

    if (command == PlaybackCommand::Info) {
        const ListRow* cur = libraryListCurrent(list);
        list.status = cur ? cur->title : "";
        step.effect = BrowseListEffect::Note;
        return step;
    }

    return step;
}

// Shape of libraryHandleInputList after the Browse branch. False means
// dispatchPlaybackInput keeps pause/skip/stop. OwnsInput runs before step so a
// live session never reaches the inner machine.
inline bool consumeBrowseListInput(LibraryList& list, bool& visible, bool playing,
                                   PlaybackCommand command, BrowseListStep& step) {
    if (!libraryListOwnsInput(visible, playing, command))
        return false;
    step = stepBrowseListInput(list, visible, playing, command);
    return true;
}

inline void putPx(std::uint8_t* line, int x, int w, const std::uint8_t rgb[3]) {
    if (!line || x < 0 || x >= w)
        return;
    line[x * 3 + 0] = rgb[0];
    line[x * 3 + 1] = rgb[1];
    line[x * 3 + 2] = rgb[2];
}

inline void fillSpan(std::uint8_t* line, int x0, int x1, int w, const std::uint8_t rgb[3]) {
    if (x0 < 0)
        x0 = 0;
    if (x1 > w)
        x1 = w;
    for (int x = x0; x < x1; ++x)
        putPx(line, x, w, rgb);
}

inline void drawGlyph(std::uint8_t* line, int lineY, int x, int y, int w, char ch,
                      const std::uint8_t fg[3]) {
    unsigned char uc = static_cast<unsigned char>(ch);
    if (uc < 0x20 || uc > 0x7E)
        uc = '?';
    const auto& glyph = kFont8x16[static_cast<std::size_t>(uc - 0x20)];
    const int gy = lineY - y;
    if (gy < 0 || gy >= kGlyphH)
        return;
    const std::uint8_t bits = glyph[static_cast<std::size_t>(gy)];
    for (int gx = 0; gx < kGlyphW; ++gx) {
        if (bits & (0x80u >> gx))
            putPx(line, x + gx, w, fg);
    }
}

inline void drawText(std::uint8_t* line, int lineY, int x, int y, int w, const std::string& text,
                     const std::uint8_t fg[3]) {
    for (char ch : text) {
        if (x >= w)
            break;
        drawGlyph(line, lineY, x, y, w, ch, fg);
        x += kGlyphW;
    }
}

// Paints the list into the left kLibraryListW columns. fbW/fbH is the idle FB
// (640x480). Columns at x >= 624 are filled with the list background and never
// receive a glyph.
inline void blitLibraryListIdle(const LibraryList& list, std::uint8_t* fb, int fbW, int fbH,
                                int strideBytes) {
    if (!fb || fbW <= 0 || fbH <= 0 || strideBytes < fbW * 3)
        return;
    const int contentW = std::min(kLibraryListW, fbW);
    const int rows = std::min(kLibraryListH, fbH);
    for (int y = 0; y < fbH; ++y) {
        std::uint8_t* line = fb + static_cast<std::size_t>(y) * static_cast<std::size_t>(strideBytes);
        fillSpan(line, 0, fbW, fbW, kBg);
        if (y >= rows)
            continue;
        if (y >= kLibraryListRuleY && y < kLibraryListRuleY + 2)
            fillSpan(line, 16, contentW - 16, contentW, kRule);

        const std::string header = listClip(listSanitize(list.header.empty() ? "Library" : list.header), 24);
        drawText(line, y, 16, kLibraryListHeaderY, contentW, header, kAmber);

        const int top = libraryListViewportTop(list);
        const int cursor = list.rows.empty()
                               ? 0
                               : std::max(0, std::min(static_cast<int>(list.rows.size()) - 1, list.cursor));
        for (int vi = 0; vi < kLibraryListViewRows; ++vi) {
            const int ri = top + vi;
            if (ri < 0 || ri >= static_cast<int>(list.rows.size()))
                continue;
            const ListRow& row = list.rows[static_cast<std::size_t>(ri)];
            const int rowY = kLibraryListY0 + vi * kLibraryListRowH;
            const bool selected = ri == cursor;
            if (selected)
                fillSpan(line, 8, contentW - 8, contentW, kAmber);
            const std::uint8_t* fg = selected ? kInk : (row.kind == ListKind::Section ? kAmber : kText);
            if (row.kind == ListKind::Section)
                drawText(line, y, kLibraryListMarkX, rowY + 4, contentW, ">", fg);
            const std::string label = listClip(listSanitize(row.title), kTitleColumns);
            drawText(line, y, kLibraryListTextX, rowY + 4, contentW, label, fg);
        }

        std::string foot = list.status;
        if (foot.empty() && !list.rows.empty())
            foot = std::to_string(cursor + 1) + "/" + std::to_string(list.rows.size());
        drawText(line, y, 16, kLibraryListFooterY, contentW, listClip(listSanitize(foot), kTitleColumns), kDim);
    }
}

inline std::vector<std::uint8_t> renderLibraryListPacked(const LibraryList& list) {
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(kLibraryListStride) * kLibraryListH);
    blitLibraryListIdle(list, rgb.data(), kLibraryListW, kLibraryListH, kLibraryListStride);
    return rgb;
}

} // namespace librarylist
} // namespace misterplex
