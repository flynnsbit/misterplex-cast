#pragma once
// Immutable RenderModel + RAM nav. Keypresses only mutate this snapshot; they never fetch.

#include "libmisterplex/browse_controller.hpp"
#include "libmisterplex/browse_shell.hpp"
#include "libmisterplex/browse_shell_layout.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace misterplex {

enum class BrowseShellScreen : int {
    Servers = 0,
    Libraries = 1,
    Titles = 2,
    Detail = 3,
    Search = 4,
    Loading = 5,
};

enum class BrowseSelectPolicy : int { Play = 0, OpenDetail = 1 };

inline BrowseSelectPolicy browseSelectOnTitle(int64_t viewOffsetMs) {
    return viewOffsetMs == 0 ? BrowseSelectPolicy::Play : BrowseSelectPolicy::OpenDetail;
}

struct BrowseServerRow {
    std::string id;
    std::string name;
    std::string locality; // local | remote
    std::string host;
    std::string status; // OK | — | …
    std::string baseUrl;
    std::string token;
    bool ok = true;
    bool lastUsed = false;
    // Ranked alternate bases from plex.tv connections (in-memory; not disk-cached).
    // Used by openLibraries transport-fail retry (cheap b). Keep last so brace-inits stay valid.
    std::vector<std::string> connectionBases;
};

struct BrowseLibraryRow {
    std::string key;
    std::string title;
    std::string type; // movie | show
    int count = 0;
};

struct BrowseTitleRow {
    std::string ratingKey;
    std::string key;
    std::string title;
    std::string year;
    std::string type;
    std::string summary;
    std::string thumb;
    int runtimeMs = 0;
    int64_t viewOffsetMs = 0;
    int viewCount = 0;
    float progress = 0.f; // 0..1
};

struct BrowseKbKey {
    std::string label;
    char ch = 0; // 0 = special
    enum class Kind : int { Letter, Digit, Space, Backspace, Clear, Done };
    Kind kind = Kind::Letter;
};

struct BrowseSearchUi {
    std::string query;
    std::string sectionId; // empty = all libraries on this server
    std::string scopeLabel = "All libraries";
    int64_t lastChangeMs = 0;
    bool pending = false;
    int kbIndex = 3; // 'D' in mockups
    int resultFocus = 0;
    int resultScroll = 0;
    enum class Region : int { Field, Keyboard, Results };
    Region region = Region::Keyboard;
    std::vector<BrowseKbKey> keys;
    std::vector<BrowseTitleRow> results;
};

struct BrowseDetailUi {
    BrowseTitleRow item;
    int button = 0; // 0 = Resume (if in progress) else Play
    bool inProgress = false;
    bool fromSearch = false;
};

// Immutable paint snapshot. Published under the controller mutex.
struct BrowseRenderModel {
    BrowseShellScreen screen = BrowseShellScreen::Servers;
    BrowseViewMode view = BrowseViewMode::List;
    int w = 640;
    int h = 480;
    int focus = 0;
    int scroll = 0;
    int viewFocus[2] = {0, 0};
    int viewScroll[2] = {0, 0};
    int loadingPct = 0;
    std::string header;
    std::string headerRight;
    std::string footer;
    std::string status;
    std::string serverName;
    std::string libraryName;
    int totalTitles = 0;
    bool childList = false;
    bool searchResults = false;
    int serverFocus = 0;
    int libraryFocus = 0;
    std::vector<BrowseServerRow> servers;
    std::vector<BrowseLibraryRow> libraries;
    std::vector<BrowseTitleRow> titles;
    BrowseSearchUi search;
    BrowseDetailUi detail;
    uint64_t generation = 0;
    int fetchCount = 0; // copied for tests; nav must not increment it
};

inline const char* browseShellScreenName(BrowseShellScreen s) {
    switch (s) {
    case BrowseShellScreen::Servers:
        return "servers";
    case BrowseShellScreen::Libraries:
        return "libraries";
    case BrowseShellScreen::Titles:
        return "titles";
    case BrowseShellScreen::Detail:
        return "detail";
    case BrowseShellScreen::Search:
        return "search";
    case BrowseShellScreen::Loading:
        return "loading";
    }
    return "?";
}

inline std::vector<BrowseKbKey> browseSearchKeys(const BrowseShellLayout&) {
    std::vector<BrowseKbKey> keys;
    auto letter = [](char c) {
        BrowseKbKey k;
        k.label.assign(1, c);
        k.ch = c;
        k.kind = BrowseKbKey::Kind::Letter;
        return k;
    };
    for (char c = 'A'; c <= 'Z'; ++c)
        keys.push_back(letter(c));
    for (char c = '0'; c <= '9'; ++c)
        keys.push_back(letter(c));
    BrowseKbKey bs;
    bs.label = "\x08";
    bs.label = "Bk";
    bs.kind = BrowseKbKey::Kind::Backspace;
    keys.push_back(bs);
    BrowseKbKey sp;
    sp.label = "Sp";
    sp.ch = ' ';
    sp.kind = BrowseKbKey::Kind::Space;
    keys.push_back(sp);
    {
        BrowseKbKey cl;
        cl.label = "Clr";
        cl.kind = BrowseKbKey::Kind::Clear;
        keys.push_back(cl);
    }
    BrowseKbKey dn;
    dn.label = "Go";
    dn.kind = BrowseKbKey::Kind::Done;
    keys.push_back(dn);
    return keys;
}

inline int browseSearchKbCols(const BrowseShellLayout& L) {
    return std::max(1, (L.w - 2 * L.margin + 3) / (L.kbCell + 3));
}

inline bool browseSearchShouldFetch(const BrowseSearchUi& s, int64_t nowMs) {
    if (!s.pending)
        return false;
    return nowMs - s.lastChangeMs >= kBrowseShellSearchDebounceMs;
}

inline void browseSearchType(BrowseSearchUi& s, char ch, int64_t nowMs) {
    if (ch == '\b') {
        if (!s.query.empty()) {
            auto start = s.query.size() - 1;
            while (start > 0 && (static_cast<unsigned char>(s.query[start]) & 0xC0) == 0x80)
                --start;
            s.query.resize(start);
        }
    } else if (ch == 0) {
        s.query.clear();
    } else if (static_cast<unsigned char>(ch) >= 32 && s.query.size() < 64) {
        if (ch >= 'a' && ch <= 'z')
            ch = static_cast<char>(ch - 'a' + 'A');
        if (static_cast<unsigned char>(ch) != 127)
            s.query.push_back(ch == ' ' ? ' ' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    s.lastChangeMs = nowMs;
    s.pending = !s.query.empty();
    if (s.query.empty()) {
        s.results.clear();
        s.pending = false;
    }
}

inline const char* browseNavActionName(BrowseOp op) {
    switch (op) {
    case BrowseOp::PageUp:
        return "pgup";
    case BrowseOp::PageDown:
        return "pgdn";
    case BrowseOp::ToggleUiSize:
    case BrowseOp::Info:
        return "toggle";
    case BrowseOp::Select:
        return "keypress";
    default:
        return "keypress";
    }
}

struct BrowseShellNav {
    BrowseRenderModel* m = nullptr;
    const BrowseShellLayout* L = nullptr;
    int64_t nowMs = 0;
    bool openedSearch = false;
    bool openedDetail = false;
    bool wantPlay = false;
    bool wantLibraries = false;
    bool wantTitles = false;
    bool wantChildren = false;
    bool wantServers = false;
    bool wantSearchFetch = false;
    bool wantSearchCommit = false;
    int playIndex = -1;
};

inline void browseEnsureFocusVisible(BrowseRenderModel& m, const BrowseShellLayout& L) {
    int n = 0;
    BrowseViewMode mode = BrowseViewMode::List;
    if (m.screen == BrowseShellScreen::Servers)
        n = static_cast<int>(m.servers.size());
    else if (m.screen == BrowseShellScreen::Libraries)
        n = static_cast<int>(m.libraries.size());
    else if (m.screen == BrowseShellScreen::Titles) {
        n = static_cast<int>(m.titles.size());
        mode = m.view;
    } else if (m.screen == BrowseShellScreen::Search)
        n = static_cast<int>(m.search.results.size());
    else
        return;
    const int vp = browseViewportCount(L, mode);
    m.focus = browseClampIndex(m.focus, n);
    if (mode == BrowseViewMode::Grid) {
        const int cols = L.gridCols;
        m.scroll = std::max(0, m.scroll / cols * cols);
        if (m.focus < m.scroll)
            m.scroll = m.focus / cols * cols;
        else if (m.focus >= m.scroll + vp)
            m.scroll = (m.focus / cols - L.gridRows + 1) * cols;
        m.scroll = std::min(m.scroll, std::max(0, ((n + cols - 1) / cols - L.gridRows) * cols));
    } else {
        m.scroll = browseScrollForFocus(m.focus, m.scroll, vp, n);
    }
}

inline void browseMoveFocus(BrowseRenderModel& m, const BrowseShellLayout& L, int delta) {
    int n = 0;
    if (m.screen == BrowseShellScreen::Servers)
        n = static_cast<int>(m.servers.size());
    else if (m.screen == BrowseShellScreen::Libraries)
        n = static_cast<int>(m.libraries.size());
    else if (m.screen == BrowseShellScreen::Titles)
        n = static_cast<int>(m.titles.size());
    else
        return;
    if (n <= 0)
        return;
    m.focus = browseClampIndex(m.focus + delta, n);
    browseEnsureFocusVisible(m, L);
}

inline void browsePage(BrowseRenderModel& m, const BrowseShellLayout& L, int dir) {
    BrowseViewMode mode = BrowseViewMode::List;
    int n = 0;
    if (m.screen == BrowseShellScreen::Servers) {
        n = static_cast<int>(m.servers.size());
    } else if (m.screen == BrowseShellScreen::Libraries) {
        n = static_cast<int>(m.libraries.size());
    } else if (m.screen == BrowseShellScreen::Titles) {
        n = static_cast<int>(m.titles.size());
        mode = m.view;
    } else if (m.screen == BrowseShellScreen::Search) {
        n = static_cast<int>(m.search.results.size());
        const int vp = L.compact ? 3 : 4;
        m.search.resultFocus = browseClampIndex(m.search.resultFocus + dir * vp, n);
        m.search.resultScroll = browseScrollForFocus(m.search.resultFocus,
            std::max(0, m.search.resultScroll + dir * vp), vp, n);
        return;
    } else {
        return;
    }
    const int d = dir * browsePageDelta(L, mode);
    m.focus = browseClampIndex(m.focus + d, n);
    m.scroll = std::max(0, m.scroll + d);
    browseEnsureFocusVisible(m, L);
}

inline void browseToggleView(BrowseRenderModel& m, const BrowseShellLayout& L) {
    if (m.screen != BrowseShellScreen::Titles && m.screen != BrowseShellScreen::Search)
        return;
    if (m.childList)
        return;
    m.viewFocus[static_cast<int>(m.view)] = m.focus;
    m.viewScroll[static_cast<int>(m.view)] = m.scroll;
    m.view = (m.view == BrowseViewMode::List) ? BrowseViewMode::Grid : BrowseViewMode::List;
    m.focus = m.viewFocus[static_cast<int>(m.view)];
    m.scroll = m.viewScroll[static_cast<int>(m.view)];
    browseEnsureFocusVisible(m, L);
}

inline bool browseApplyNav(BrowseRenderModel& m, const BrowseShellLayout& L, BrowseOp op, int64_t nowMs,
                           BrowseShellNav* out = nullptr) {
    BrowseShellNav local;
    BrowseShellNav& r = out ? *out : local;
    r = {};
    r.m = &m;
    r.L = &L;
    r.nowMs = nowMs;

    if (op == BrowseOp::Open) {
        m.screen = BrowseShellScreen::Servers;
        m.focus = m.serverFocus;
        m.scroll = 0;
        m.header = "SERVERS";
        m.headerRight = "[Search]";
        m.search.query.clear();
        m.search.pending = false;
        m.searchResults = false;
        m.childList = false;
        browseEnsureFocusVisible(m, L);
        r.wantServers = m.servers.empty();
        return true;
    }
    if (m.screen == BrowseShellScreen::Loading)
        return false;

    if (m.screen == BrowseShellScreen::Search) {
        auto& s = m.search;
        const int nKeys = static_cast<int>(s.keys.size());
        const int cols = browseSearchKbCols(L);
        if (op == BrowseOp::Back) {
            m.screen = BrowseShellScreen::Titles;
            s.query.clear();
            s.pending = false;
            s.results.clear();
            return true;
        }
        if (op == BrowseOp::Info || op == BrowseOp::ToggleUiSize) {
            browseToggleView(m, L);
            return true;
        }
        if (s.region == BrowseSearchUi::Region::Keyboard && nKeys > 0) {
            if (op == BrowseOp::Left)
                s.kbIndex = browseClampIndex(s.kbIndex - 1, nKeys);
            else if (op == BrowseOp::Right)
                s.kbIndex = browseClampIndex(s.kbIndex + 1, nKeys);
            else if (op == BrowseOp::Up) {
                if (s.kbIndex >= cols)
                    s.kbIndex -= cols;
                else
                    s.region = BrowseSearchUi::Region::Field;
            } else if (op == BrowseOp::Down) {
                if (s.kbIndex + cols < nKeys)
                    s.kbIndex += cols;
                else
                    s.region = BrowseSearchUi::Region::Results;
            } else if (op == BrowseOp::Select && s.kbIndex >= 0 && s.kbIndex < nKeys) {
                const BrowseKbKey& k = s.keys[static_cast<std::size_t>(s.kbIndex)];
                if (k.kind == BrowseKbKey::Kind::Backspace)
                    browseSearchType(s, '\b', nowMs);
                else if (k.kind == BrowseKbKey::Kind::Clear)
                    browseSearchType(s, 0, nowMs);
                else if (k.kind == BrowseKbKey::Kind::Done) {
                    r.wantSearchFetch = true;
                    r.wantSearchCommit = true;
                }
                else if (k.kind == BrowseKbKey::Kind::Space)
                    browseSearchType(s, ' ', nowMs);
                else if (k.ch)
                    browseSearchType(s, k.ch, nowMs);
                r.wantSearchFetch = r.wantSearchFetch || s.pending;
            }
            return true;
        }
        if (s.region == BrowseSearchUi::Region::Field) {
            if (op == BrowseOp::Down)
                s.region = BrowseSearchUi::Region::Keyboard;
            else if (op == BrowseOp::Select) {
                r.wantSearchFetch = true;
                r.wantSearchCommit = true;
            }
            return true;
        }
        if (s.region == BrowseSearchUi::Region::Results) {
            const int n = static_cast<int>(s.results.size());
            // Vertical list: Left/Right alias Up/Down (plexui MUT_RC1037_REMOTE_NOLR / V2 Right→Down).
            if (op == BrowseOp::Up || op == BrowseOp::Left) {
                if (s.resultFocus > 0)
                    s.resultFocus--;
                else
                    s.region = BrowseSearchUi::Region::Keyboard;
            } else if (op == BrowseOp::Down || op == BrowseOp::Right)
                s.resultFocus = browseClampIndex(s.resultFocus + 1, n);
            else if (op == BrowseOp::PageUp)
                browsePage(m, L, -1);
            else if (op == BrowseOp::PageDown)
                browsePage(m, L, +1);
            else if (op == BrowseOp::Select && n > 0) {
                const int i = browseClampIndex(s.resultFocus, n);
                const auto& t = s.results[static_cast<std::size_t>(i)];
                if (t.type == "show" || t.type == "season") {
                    r.wantChildren = true;
                    r.playIndex = i;
                } else if (browseSelectOnTitle(t.viewOffsetMs) == BrowseSelectPolicy::Play) {
                    r.wantPlay = true;
                    r.playIndex = i;
                } else {
                    m.detail.item = t;
                    m.detail.inProgress = t.viewOffsetMs > 0;
                    m.detail.fromSearch = true;
                    m.detail.button = 0;
                    m.screen = BrowseShellScreen::Detail;
                    r.openedDetail = true;
                }
            }
            s.resultScroll = browseScrollForFocus(s.resultFocus, s.resultScroll, L.compact ? 3 : 4, n);
            return true;
        }
        return true;
    }

    if (m.screen == BrowseShellScreen::Detail) {
        const int nBtn = m.detail.inProgress ? 2 : 1;
        if (op == BrowseOp::Back) {
            m.screen = m.detail.fromSearch ? BrowseShellScreen::Search : BrowseShellScreen::Titles;
            return true;
        }
        if (op == BrowseOp::Left)
            m.detail.button = browseClampIndex(m.detail.button - 1, nBtn);
        else if (op == BrowseOp::Right)
            m.detail.button = browseClampIndex(m.detail.button + 1, nBtn);
        else if (op == BrowseOp::Select) {
            r.wantPlay = true;
            r.playIndex = m.focus;
        }
        return true;
    }

    switch (op) {
    case BrowseOp::Up:
        browseMoveFocus(m, L, m.screen == BrowseShellScreen::Titles && m.view == BrowseViewMode::Grid
                                 ? -L.gridCols : -1);
        return true;
    case BrowseOp::Down:
        browseMoveFocus(m, L, m.screen == BrowseShellScreen::Titles && m.view == BrowseViewMode::Grid
                                 ? L.gridCols : 1);
        return true;
    case BrowseOp::PageUp:
        browsePage(m, L, -1);
        return true;
    case BrowseOp::PageDown:
        browsePage(m, L, +1);
        return true;
    case BrowseOp::Left:
        // Vertical screens (Servers/Libraries/Titles-List) alias Left like Up (±1).
        // Titles+Grid keeps column −1 (same delta). Matches plexui MUT_RC1037_REMOTE_NOLR.
        browseMoveFocus(m, L, -1);
        return true;
    case BrowseOp::Right:
        // Vertical screens alias Right like Down (±1); Titles+Grid keeps column +1.
        browseMoveFocus(m, L, +1);
        return true;
    case BrowseOp::Info:
    case BrowseOp::ToggleUiSize:
        if (m.screen == BrowseShellScreen::Titles)
            browseToggleView(m, L);
        return true;
    case BrowseOp::Back:
        if (m.screen == BrowseShellScreen::Titles) {
            m.screen = BrowseShellScreen::Libraries;
            m.focus = m.libraryFocus;
            m.scroll = 0;
            m.header = "LIBRARIES";
            // Prefer explicit headerRight; paint falls back to serverName when empty.
            if (m.headerRight.empty() && !m.serverName.empty())
                m.headerRight = m.serverName;
        } else if (m.screen == BrowseShellScreen::Libraries) {
            m.screen = BrowseShellScreen::Servers;
            m.focus = m.serverFocus;
            m.scroll = 0;
            // paintServers uses fallback(m.header,...) so a leftover LIBRARIES /
            // server address string would otherwise stick in the Servers header.
            m.header = "SERVERS";
            m.headerRight = "[Search]";
        }
        browseEnsureFocusVisible(m, L);
        return true;
    case BrowseOp::Select:
        if (m.screen == BrowseShellScreen::Servers) {
            r.wantLibraries = true;
        } else if (m.screen == BrowseShellScreen::Libraries) {
            r.wantTitles = true;
        } else if (m.screen == BrowseShellScreen::Titles && !m.titles.empty()) {
            const int i = browseClampIndex(m.focus, static_cast<int>(m.titles.size()));
            const auto& t = m.titles[static_cast<std::size_t>(i)];
            if (t.type == "show" || t.type == "season") {
                r.wantChildren = true;
                r.playIndex = i;
            } else if (browseSelectOnTitle(t.viewOffsetMs) == BrowseSelectPolicy::Play) {
                r.wantPlay = true;
                r.playIndex = i;
            } else {
                m.detail.item = t;
                m.detail.inProgress = t.viewOffsetMs > 0;
                m.detail.fromSearch = false;
                m.detail.button = 0;
                m.screen = BrowseShellScreen::Detail;
                r.openedDetail = true;
            }
        }
        return true;
    default:
        return false;
    }
}

inline int browsePrefetchCount(int viewport, int totalSize) {
    const int two = std::max(60, viewport * 2);
    if (totalSize > 0 && totalSize <= kBrowseShellPrefetchAllMax)
        return totalSize;
    if (browsePrefetchAllEnabled() && totalSize > 0)
        return totalSize;
    return two;
}

inline void browseSortServers(std::vector<BrowseServerRow>& rows) {
    std::stable_sort(rows.begin(), rows.end(), [](const BrowseServerRow& a, const BrowseServerRow& b) {
        if (a.lastUsed != b.lastUsed)
            return a.lastUsed;
        const bool aLocal = a.locality == "local";
        const bool bLocal = b.locality == "local";
        if (aLocal != bLocal)
            return aLocal;
        return a.name < b.name;
    });
}

// Select / refresh must not sit on a "…" or empty-base row when an OK neighbor exists
// (glass 63332105 title-play: ENTER on lab-pms "…" never left Servers).
inline bool browseServerRowUsable(const BrowseServerRow& row) {
    return row.ok || !row.baseUrl.empty();
}

inline int browseFirstUsableServerIndex(const std::vector<BrowseServerRow>& rows) {
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (browseServerRowUsable(rows[i]))
            return static_cast<int>(i);
    }
    return -1;
}

inline void browseSnapFocusToUsableServer(BrowseRenderModel& m) {
    if (m.servers.empty())
        return;
    const int n = static_cast<int>(m.servers.size());
    const int cur = browseClampIndex(m.focus, n);
    if (browseServerRowUsable(m.servers[static_cast<std::size_t>(cur)]))
        return;
    int pick = -1;
    for (int i = 0; i < n; ++i) {
        if (m.servers[static_cast<std::size_t>(i)].lastUsed &&
            browseServerRowUsable(m.servers[static_cast<std::size_t>(i)])) {
            pick = i;
            break;
        }
    }
    if (pick < 0)
        pick = browseFirstUsableServerIndex(m.servers);
    if (pick >= 0)
        m.focus = pick;
}

} // namespace misterplex
