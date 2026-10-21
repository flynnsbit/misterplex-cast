#pragma once
// On-glass PMS library overlay. No FPGA exec — daemon paints + playMedia.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace misterplex {

struct LibraryRow {
    std::string title;
    std::string key;
    std::string ratingKey;
    std::string type;
    bool directory = false;
    // MPX_BROWSE_V2 only (browse_v2.hpp parseBrowseXml); the V1 parser leaves them empty.
    bool fromMenu = false;   // a row of the mpx-menu helper (its keys are helper paths)
    std::string mpxServer;   // helper server slug ("local", "s<7 hex>")
    std::string mpxSource;   // "local" (PMS play by ratingKey) or "ladder" (resolve on the helper)
    std::string mpxKind;     // "info"/"offline": shown, never activated
    bool offsetPresent = false; // mpxOffsetMs given (0 = play from start)
    int64_t offsetMs = 0;
    int mpxPct = -1;            // /a1 helper progress 1..99 (mpxPct); -1 = none
};

// Library XML (PMS and mpx-menu MediaContainer) start-tag scanning. A '>' or "/>" inside a
// quoted attribute value belongs to the value (XML allows a literal '>' there), attribute names
// match exactly, and values are entity-decoded. So rows such as title="NEXT >" and
// title="&lt; PREV" read the way a real XML parser reads them (dbcc42fa cut the tag at the
// first '>' and returned values undecoded).

inline bool xmlSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Index of the '>' that closes the start tag at p (xml[p] == '<'), skipping '...' and "..."
// attribute values. npos when the tag is not terminated.
inline size_t xmlTagEnd(const std::string& xml, size_t p) {
    char quote = 0;
    for (size_t i = p + 1; i < xml.size(); ++i) {
        const char c = xml[i];
        if (quote) {
            if (c == quote)
                quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '>') {
            return i;
        }
    }
    return std::string::npos;
}

inline void xmlAppendUtf8(std::string& out, unsigned long cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Decodes the five predefined entities and &#N; / &#xH; character references. Anything else
// (a bare '&', an unknown or malformed reference) is kept verbatim.
inline std::string xmlDecode(const std::string& s) {
    if (s.find('&') == std::string::npos)
        return s;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const auto semi = s[i] == '&' ? s.find(';', i + 1) : std::string::npos;
        if (semi == std::string::npos || semi - i > 12) {
            out += s[i];
            continue;
        }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        unsigned long cp = 0;
        bool ok = true;
        if (ent == "lt") {
            cp = '<';
        } else if (ent == "gt") {
            cp = '>';
        } else if (ent == "amp") {
            cp = '&';
        } else if (ent == "quot") {
            cp = '"';
        } else if (ent == "apos") {
            cp = '\'';
        } else if (ent.size() >= 2 && ent[0] == '#') {
            const bool hex = ent[1] == 'x' || ent[1] == 'X';
            const size_t d0 = hex ? 2 : 1;
            ok = ent.size() > d0;
            for (size_t k = d0; ok && k < ent.size(); ++k) {
                const char c = ent[k];
                int v = -1;
                if (c >= '0' && c <= '9')
                    v = c - '0';
                else if (hex && c >= 'a' && c <= 'f')
                    v = c - 'a' + 10;
                else if (hex && c >= 'A' && c <= 'F')
                    v = c - 'A' + 10;
                ok = v >= 0;
                cp = cp * (hex ? 16 : 10) + static_cast<unsigned long>(v < 0 ? 0 : v);
                ok = ok && cp <= 0x10FFFF;
            }
            ok = ok && cp != 0 && !(cp >= 0xD800 && cp <= 0xDFFF);
        } else {
            ok = false;
        }
        if (!ok) {
            out += s[i];
            continue;
        }
        xmlAppendUtf8(out, cp);
        i = semi;
    }
    return out;
}

// Decoded value of attribute `name` in the start tag `tag` ("<Elem a=\"v\" b='w' ...>").
// Empty when the attribute is absent.
inline std::string xmlAttr(const std::string& tag, const char* name) {
    const size_t want = std::strlen(name);
    const size_t n = tag.size();
    size_t i = (n > 0 && tag[0] == '<') ? 1 : 0;
    while (i < n && !xmlSpace(tag[i]) && tag[i] != '>' && tag[i] != '/')
        ++i;  // element name
    while (i < n) {
        while (i < n && xmlSpace(tag[i]))
            ++i;
        if (i >= n || tag[i] == '>' || tag[i] == '/')
            break;
        const size_t ns = i;
        while (i < n && !xmlSpace(tag[i]) && tag[i] != '=' && tag[i] != '>' && tag[i] != '/')
            ++i;
        const size_t ne = i;
        while (i < n && xmlSpace(tag[i]))
            ++i;
        if (i >= n || tag[i] != '=') {
            if (ne == ns)
                break;
            continue;  // not name="value": skip the token
        }
        ++i;
        while (i < n && xmlSpace(tag[i]))
            ++i;
        if (i >= n || (tag[i] != '"' && tag[i] != '\''))
            break;  // unquoted value: not XML
        const char q = tag[i++];
        const auto ve = tag.find(q, i);
        if (ve == std::string::npos)
            break;
        if (ne - ns == want && tag.compare(ns, want, name) == 0)
            return xmlDecode(tag.substr(i, ve - i));
        i = ve + 1;
    }
    return {};
}

inline void appendTags(const std::string& xml, const char* open, bool directory,
                       std::vector<LibraryRow>& out) {
    const size_t olen = std::strlen(open);
    size_t i = 0;
    while (i < xml.size()) {
        const auto p = xml.find(open, i);
        if (p == std::string::npos)
            break;
        if (p + olen < xml.size() && !xmlSpace(xml[p + olen]) && xml[p + olen] != '/' &&
            xml[p + olen] != '>') {
            i = p + olen;  // a longer element name, e.g. <Videos
            continue;
        }
        const auto e = xmlTagEnd(xml, p);
        if (e == std::string::npos)
            break;
        const std::string tag = xml.substr(p, e - p + 1);
        LibraryRow r;
        r.title = xmlAttr(tag, "title");
        r.key = xmlAttr(tag, "key");
        r.ratingKey = xmlAttr(tag, "ratingKey");
        r.type = xmlAttr(tag, "type");
        r.directory = directory;
        if (!r.title.empty() || !r.key.empty() || !r.ratingKey.empty())
            out.push_back(std::move(r));
        i = e + 1;
    }
}

inline std::vector<LibraryRow> parseLibraryXml(const std::string& xml) {
    std::vector<LibraryRow> rows;
    appendTags(xml, "<Directory", true, rows);
    appendTags(xml, "<Video", false, rows);
    appendTags(xml, "<Track", false, rows);
    return rows;
}

inline std::string libraryListPath(const LibraryRow& row) {
    if (row.key.empty())
        return {};
    if (row.key[0] == '/')
        return row.key;
    return "/library/sections/" + row.key + "/all";
}

struct LibraryBrowser {
    bool visible = false;
    std::vector<std::vector<LibraryRow>> stack;
    std::vector<int> cursors;
    std::string status;

    // A row the cursor may rest on: every row but the helper's info/offline/header rows, the rows
    // the R1 renderer draws as RowKind::Info and never highlights (browse_r1_adapter.hpp r1Row,
    // browse_render.hpp selectable). The cursor stays on these rows only, so the highlighted row
    // is the row Select acts on (review 09:38). Legacy PMS rows carry no mpxKind: all selectable.
    static bool selectable(const LibraryRow& r) {
        return r.mpxKind != "info" && r.mpxKind != "offline" && r.mpxKind != "header";
    }

    void reset() {
        visible = false;
        stack.clear();
        cursors.clear();
        status.clear();
    }

    void show(std::vector<LibraryRow> root, std::string why = {}) {
        stack.clear();
        cursors.clear();
        stack.push_back(std::move(root));
        cursors.push_back(0);
        visible = true;
        status = std::move(why);
        clamp();
    }

    void hide() { visible = false; }

    bool empty() const { return stack.empty() || stack.back().empty(); }

    int cursor() const { return cursors.empty() ? 0 : cursors.back(); }

    const LibraryRow* current() const {
        if (stack.empty() || cursors.empty())
            return nullptr;
        const auto& rows = stack.back();
        const int c = cursors.back();
        if (c < 0 || c >= static_cast<int>(rows.size()))
            return nullptr;
        return &rows[static_cast<size_t>(c)];
    }

    void clamp() {
        if (stack.empty())
            return;
        if (cursors.size() != stack.size())
            cursors.resize(stack.size(), 0);
        auto& c = cursors.back();
        const auto& rows = stack.back();
        const int n = static_cast<int>(rows.size());
        if (n <= 0)
            c = 0;
        else if (c < 0)
            c = 0;
        else if (c >= n)
            c = n - 1;
        if (n <= 0 || selectable(rows[static_cast<size_t>(c)]))
            return;
        // Onto a selectable row: forward first, then back (browse_render.hpp normalizedCursor).
        for (int i = c + 1; i < n; ++i)
            if (selectable(rows[static_cast<size_t>(i)])) {
                c = i;
                return;
            }
        for (int i = c - 1; i >= 0; --i)
            if (selectable(rows[static_cast<size_t>(i)])) {
                c = i;
                return;
            }
    }

    // |delta| selectable rows up or down, wrapping. With every row selectable this is the plain
    // (cursor + delta) mod n; a level with no selectable row keeps its cursor.
    void move(int delta) {
        if (!visible || stack.empty())
            return;
        clamp();
        const auto& rows = stack.back();
        const int n = static_cast<int>(rows.size());
        if (n <= 0)
            return;
        const int dir = delta < 0 ? -1 : 1;
        int c = cursors.back();
        for (int k = delta < 0 ? -delta : delta; k > 0; --k) {
            int next = c;
            for (int i = 0; i < n; ++i) {
                next = (next + dir + n) % n;
                if (selectable(rows[static_cast<size_t>(next)]))
                    break;
            }
            c = next;
        }
        cursors.back() = c;
    }

    void push(std::vector<LibraryRow> child) {
        stack.push_back(std::move(child));
        cursors.push_back(0);
        clamp();
    }

    bool pop() {
        if (stack.size() <= 1) {
            hide();
            return false;
        }
        stack.pop_back();
        cursors.pop_back();
        return true;
    }
};

// 5×7 caps, same pitch as playback_overlay glyphs.
inline const uint8_t* libraryGlyph(char ch) {
    static constexpr uint8_t sp[7] = {0, 0, 0, 0, 0, 0, 0};
    static constexpr uint8_t g[27][7] = {
        {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, // A
        {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}, // B
        {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}, // C
        {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}, // D
        {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}, // E
        {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}, // F
        {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}, // G
        {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, // H
        {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}, // I
        {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0e}, // J
        {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, // K
        {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}, // L
        {0x11, 0x1b, 0x15, 0x11, 0x11, 0x11, 0x11}, // M
        {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, // N
        {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}, // O
        {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}, // P
        {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}, // Q
        {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}, // R
        {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}, // S
        {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, // T
        {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}, // U
        {0x11, 0x11, 0x11, 0x11, 0x0a, 0x0a, 0x04}, // V
        {0x11, 0x11, 0x11, 0x15, 0x15, 0x1b, 0x11}, // W
        {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}, // X
        {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}, // Y
        {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}, // Z
    };
    static constexpr uint8_t d[10][7] = {
        {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}, {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},
        {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}, {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e},
        {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}, {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e},
        {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}, {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}, {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c},
    };
    static constexpr uint8_t gt[7] = {0x04, 0x08, 0x10, 0x08, 0x04, 0x00, 0x00}; // >
    static constexpr uint8_t dash[7] = {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00};
    static constexpr uint8_t dot[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04};
    static constexpr uint8_t qmark[7] = {0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};
    static constexpr uint8_t slash[7] = {0x01, 0x02, 0x04, 0x04, 0x08, 0x10, 0x10};
    static constexpr uint8_t colon[7] = {0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00};
    static constexpr uint8_t apos[7] = {0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
    if (ch >= 'a' && ch <= 'z')
        ch = static_cast<char>(ch - 'a' + 'A');
    if (ch >= 'A' && ch <= 'Z')
        return g[ch - 'A'];
    if (ch >= '0' && ch <= '9')
        return d[ch - '0'];
    if (ch == '>')
        return gt;
    if (ch == '-' || ch == '_')
        return dash;
    if (ch == '.' || ch == ',')
        return dot;
    if (ch == '?')
        return qmark;
    if (ch == '/')
        return slash;
    if (ch == ':' || ch == ';')
        return colon;
    if (ch == '\'' || ch == '`')
        return apos;
    return sp;
}

// MPX_BROWSE_V2 rendering only: '<' and '>' drawn the right way round (libraryGlyph's '>'
// cell is a '<'), plus the characters the helper's labels and PMS titles use that the V1
// table renders blank. Anything else falls back to libraryGlyph.
inline const uint8_t* libraryGlyphExt(char ch) {
    static constexpr uint8_t lt[7] = {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02};
    static constexpr uint8_t gt[7] = {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08};
    static constexpr uint8_t pct[7] = {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03};
    static constexpr uint8_t lpar[7] = {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02};
    static constexpr uint8_t rpar[7] = {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08};
    static constexpr uint8_t amp[7] = {0x0c, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0d};
    static constexpr uint8_t plus[7] = {0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00};
    static constexpr uint8_t bang[7] = {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04};
    static constexpr uint8_t hash[7] = {0x0a, 0x0a, 0x1f, 0x0a, 0x1f, 0x0a, 0x0a};
    static constexpr uint8_t eq[7] = {0x00, 0x00, 0x1f, 0x00, 0x1f, 0x00, 0x00};
    static constexpr uint8_t comma[7] = {0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x08};
    switch (ch) {
    case '<': return lt;
    case '>': return gt;
    case '%': return pct;
    case '(':
    case '[': return lpar;
    case ')':
    case ']': return rpar;
    case '&': return amp;
    case '+': return plus;
    case '!': return bang;
    case '#': return hash;
    case '=': return eq;
    case ',': return comma;
    default: return libraryGlyph(ch);
    }
}

inline void libraryPutY(uint8_t* y, int w, int h, int x, int y0, uint8_t v) {
    if (x < 0 || y0 < 0 || x >= w || y0 >= h)
        return;
    y[static_cast<size_t>(y0) * static_cast<size_t>(w) + static_cast<size_t>(x)] = v;
}

inline void libraryDrawTextGlyphs(uint8_t* y, int w, int h, int x, int y0, const char* text,
                                  int scale, uint8_t ink, bool extendedGlyphs) {
    if (!y || !text || scale < 1)
        return;
    for (const char* p = text; *p; ++p) {
        const uint8_t* g = extendedGlyphs ? libraryGlyphExt(*p) : libraryGlyph(*p);
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (g[row] & (1u << (4 - col)))
                    for (int dy = 0; dy < scale; ++dy)
                        for (int dx = 0; dx < scale; ++dx)
                            libraryPutY(y, w, h, x + col * scale + dx, y0 + row * scale + dy,
                                        ink);
        x += 6 * scale;
    }
}

inline void libraryDrawText(uint8_t* y, int w, int h, int x, int y0, const char* text,
                            int scale, uint8_t ink) {
    libraryDrawTextGlyphs(y, w, h, x, y0, text, scale, ink, false);
}

// The list geometry renderLibraryI420 uses: glyph scale, rows on screen, characters per line.
inline int libraryRenderScale(int w) { return (w >= 1280) ? 3 : ((w >= 640) ? 2 : 1); }
inline int libraryRowsOnScreen(int w, int h) {
    const int scale = libraryRenderScale(w);
    const int rowH = 9 * scale;
    const int yList = 8 + 22 * scale;
    return std::max(1, (h - yList - 8) / rowH);
}
inline int libraryCharsPerLine(int w) {
    const int scale = libraryRenderScale(w);
    return std::max(1, (w - 8) / (6 * scale));
}

// title: the header line; extendedGlyphs: libraryGlyphExt (MPX_BROWSE_V2). renderLibraryI420
// below is this with "PLEX LIBRARY" and the V1 glyph table.
inline void renderLibraryI420Titled(uint8_t* i420, int w, int h, const LibraryBrowser& br,
                                    const char* title, bool extendedGlyphs) {
    if (!i420 || w < 64 || h < 64 || (w & 1) || (h & 1) || !br.visible)
        return;
    const size_t ysz = static_cast<size_t>(w) * static_cast<size_t>(h);
    uint8_t* y = i420;
    uint8_t* u = i420 + ysz;
    uint8_t* v = u + ysz / 4;
    std::memset(y, 0x1a, ysz);
    std::memset(u, 128, ysz / 4);
    std::memset(v, 128, ysz / 4);

    const int scale = libraryRenderScale(w);
    libraryDrawTextGlyphs(y, w, h, 8, 8, title ? title : "", scale, 0xE0, extendedGlyphs);
    if (!br.status.empty())
        libraryDrawTextGlyphs(y, w, h, 8, 8 + 10 * scale, br.status.c_str(), scale, 0x90,
                              extendedGlyphs);

    const int rowH = 9 * scale;
    const int yList = 8 + 22 * scale;
    const int maxRows = libraryRowsOnScreen(w, h);
    if (br.stack.empty())
        return;
    const auto& rows = br.stack.back();
    const int cur = br.cursor();
    int start = 0;
    if (cur >= maxRows)
        start = cur - maxRows + 1;
    for (int i = 0; i < maxRows; ++i) {
        const int idx = start + i;
        if (idx >= static_cast<int>(rows.size()))
            break;
        const int yy = yList + i * rowH;
        const bool hi = (idx == cur);
        if (hi) {
            for (int r = 0; r < rowH - scale; ++r)
                for (int x = 4; x < w - 4; ++x)
                    libraryPutY(y, w, h, x, yy + r, 0x40);
        }
        char line[96];
        const auto& row = rows[static_cast<size_t>(idx)];
        const char mark = row.directory ? '>' : ' ';
        std::snprintf(line, sizeof(line), "%c %s", mark, row.title.empty() ? "?" : row.title.c_str());
        libraryDrawTextGlyphs(y, w, h, 8, yy + scale, line, scale, hi ? 0xF0 : 0xC8,
                              extendedGlyphs);
    }
}

inline void renderLibraryI420(uint8_t* i420, int w, int h, const LibraryBrowser& br) {
    renderLibraryI420Titled(i420, w, h, br, "PLEX LIBRARY", false);
}

} // namespace misterplex
