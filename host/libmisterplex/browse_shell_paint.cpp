#include "libmisterplex/browse_shell_paint.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <charconv>
#include <cstring>
#include <vector>

namespace misterplex {
namespace {

class PaintText {
public:
    PaintText() = default;
    PaintText(std::string_view s) { *this += s; }
    PaintText& operator+=(std::string_view s) {
        auto n = std::min(s.size(), sizeof data_ - size_);
        if (n < s.size())
            while (n && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80)
                --n;
        if (n)
            std::memcpy(data_ + size_, s.data(), n);
        size_ += n;
        return *this;
    }
    operator std::string_view() const { return {data_, size_}; }
private:
    char data_[2048];
    std::size_t size_ = 0;
};

PaintText number(int n) {
    char buf[32];
    const auto result = std::to_chars(buf, buf + sizeof buf, n);
    return PaintText(std::string_view(buf, static_cast<std::size_t>(result.ptr - buf)));
}

thread_local BrowseRect clip;

std::string_view fallback(std::string_view value, std::string_view otherwise) {
    return value.empty() ? otherwise : value;
}

bool overlaps(int x, int y, int w, int h) {
    return x < clip.x1 && x + w > clip.x0 && y < clip.y1 && y + h > clip.y0;
}

constexpr std::uint8_t BG_TOP[3] = {0x12, 0x16, 0x1C};
constexpr std::uint8_t BG_BOT[3] = {0x0B, 0x0E, 0x12};
constexpr std::uint8_t TEXT[3] = {0xE8, 0xE8, 0xE8};
constexpr std::uint8_t DIM[3] = {0x9A, 0xA0, 0xA6};
constexpr std::uint8_t AMBER[3] = {0xE5, 0xA0, 0x0D};
constexpr std::uint8_t RULE[3] = {0x2A, 0x30, 0x38};
constexpr std::uint8_t ERRORC[3] = {0xFF, 0x6B, 0x5B};
constexpr std::uint8_t OKC[3] = {0x6B, 0xC8, 0x7A};
constexpr std::uint8_t PANEL[3] = {0x16, 0x1A, 0x22};
constexpr std::uint8_t FOCUS_BAR[3] = {0x2A, 0x22, 0x10};

const unsigned char kPosterColors[][3] = {
    {0x3A, 0x4A, 0x6A}, {0x5A, 0x3A, 0x2A}, {0x2A, 0x4A, 0x3A}, {0x4A, 0x2A, 0x4A},
    {0x3A, 0x3A, 0x2A}, {0x2A, 0x3A, 0x5A}, {0x5A, 0x4A, 0x2A}, {0x3A, 0x2A, 0x2A},
    {0x2A, 0x4A, 0x4A}, {0x4A, 0x3A, 0x3A}, {0x2A, 0x2A, 0x4A}, {0x4A, 0x4A, 0x3A},
};

inline std::uint8_t* px(std::uint8_t* rgb, int w, int x, int y) {
    return rgb + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 3u;
}

void fillBg(std::uint8_t* rgb, int w, int h) {
    thread_local int tableH = 0;
    thread_local std::vector<std::array<std::uint8_t, 3>> colors;
    if (tableH != h) {
        colors.resize(static_cast<std::size_t>(h));
        for (int y = 0; y < h; ++y)
            for (int c = 0; c < 3; ++c)
                colors[y][c] = BG_TOP[c] - ((BG_TOP[c] - BG_BOT[c]) * y + std::max(1, h - 1) - 1) /
                                           std::max(1, h - 1);
        tableH = h;
    }
    for (int y = clip.y0; y < clip.y1; ++y) {
        const auto* color = colors[static_cast<std::size_t>(y)].data();
        std::uint8_t* row = rgb + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 3u;
        for (int x = clip.x0; x < clip.x1; ++x) {
            row[x * 3 + 0] = color[0];
            row[x * 3 + 1] = color[1];
            row[x * 3 + 2] = color[2];
        }
    }
}

void fillRect(std::uint8_t* rgb, int w, int, int x0, int y0, int x1, int y1, const std::uint8_t c[3]) {
    x0 = std::max(clip.x0, x0);
    y0 = std::max(clip.y0, y0);
    x1 = std::min(clip.x1, x1);
    y1 = std::min(clip.y1, y1);
    for (int y = y0; y < y1; ++y) {
        std::uint8_t* row = rgb + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 3u;
        for (int x = x0; x < x1; ++x) {
            row[x * 3 + 0] = c[0];
            row[x * 3 + 1] = c[1];
            row[x * 3 + 2] = c[2];
        }
    }
}

void hline(std::uint8_t* rgb, int w, int, int x0, int x1, int y, const std::uint8_t c[3]) {
    if (y < clip.y0 || y >= clip.y1)
        return;
    x0 = std::max(clip.x0, x0);
    x1 = std::min(clip.x1, x1);
    std::uint8_t* row = rgb + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 3u;
    for (int x = x0; x < x1; ++x) {
        row[x * 3 + 0] = c[0];
        row[x * 3 + 1] = c[1];
        row[x * 3 + 2] = c[2];
    }
}

void strokeRect(std::uint8_t* rgb, int w, int h, int x0, int y0, int x1, int y1, const std::uint8_t c[3],
                int t) {
    for (int i = 0; i < t; ++i)
        fillRect(rgb, w, h, x0 - i, y0 - i, x1 + i, y0 - i + 1, c);
    for (int i = 0; i < t; ++i)
        fillRect(rgb, w, h, x0 - i, y1 + i - 1, x1 + i, y1 + i, c);
    for (int i = 0; i < t; ++i)
        fillRect(rgb, w, h, x0 - i, y0, x0 - i + 1, y1, c);
    for (int i = 0; i < t; ++i)
        fillRect(rgb, w, h, x1 + i - 1, y0, x1 + i, y1, c);
}

void blitRgb(std::uint8_t* rgb, int w, int, int x, int y, int tw, int th, const std::uint8_t* src) {
    for (int yy = 0; yy < th; ++yy) {
        const int dy = y + yy;
        if (dy < clip.y0 || dy >= clip.y1)
            continue;
        for (int xx = std::max(0, clip.x0 - x); xx < std::min(tw, clip.x1 - x); ++xx) {
            const int dx = x + xx;
            const std::uint8_t* s = src + (static_cast<std::size_t>(yy) * tw + xx) * 3u;
            std::uint8_t* d = px(rgb, w, dx, dy);
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }
}

// FontSet paints RGBA. We keep a scratch RGBA canvas the size of the frame.
struct Rgba {
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;
    void ensure(int nw, int nh) {
        if (w == nw && h == nh && px.size() == static_cast<std::size_t>(nw) * nh * 4u)
            return;
        w = nw;
        h = nh;
        px.assign(static_cast<std::size_t>(nw) * nh * 4u, 0);
    }
    void clear() {
        for (int y = clip.y0; y < clip.y1; ++y)
            std::memset(px.data() + (static_cast<std::size_t>(y) * w + clip.x0) * 4, 0,
                        static_cast<std::size_t>(clip.x1 - clip.x0) * 4);
    }
};

thread_local Rgba g_rgba;

void text(const plexui::FontSet& font, int x, int y, std::string_view s, int size, bool bold,
          const std::uint8_t c[3]) {
    if (s.empty())
        return;
    if (y + std::max(32, size * 2) < clip.y0 || y - size > clip.y1)
        return;
    font.drawText(g_rgba.px.data(), g_rgba.w * 4, g_rgba.w, g_rgba.h, static_cast<float>(x),
                  static_cast<float>(y), s, static_cast<float>(size), bold, c[0], c[1], c[2], 255,
                  clip.x0, clip.y0, clip.x1, clip.y1);
}

int textW(const plexui::FontSet& font, std::string_view s, int size, bool bold) {
    return static_cast<int>(std::lround(font.measureText(s, static_cast<float>(size), bold)));
}

PaintText ellipsize(const plexui::FontSet& font, std::string_view s, int size, bool bold, int maxW) {
    if (textW(font, s, size, bold) <= maxW)
        return PaintText(s);
    const int dots = textW(font, "\xE2\x80\xA6", size, bold);
    if (dots > maxW)
        return {};
    const auto end = font.fittingPrefix(s, static_cast<float>(size), bold, static_cast<float>(maxW - dots));
    PaintText out(s.substr(0, end));
    out += "\xE2\x80\xA6";
    return out;
}

void compositeText(std::uint8_t* rgb, int w, int) {
    for (int y = clip.y0; y < clip.y1; ++y) {
        const std::uint8_t* src = g_rgba.px.data() + static_cast<std::size_t>(y) * w * 4u;
        std::uint8_t* dst = rgb + static_cast<std::size_t>(y) * w * 3u;
        for (int x = clip.x0; x < clip.x1; ++x) {
            const std::uint8_t a = src[x * 4 + 3];
            if (!a)
                continue;
            if (a == 255) {
                dst[x * 3 + 0] = src[x * 4 + 0];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = src[x * 4 + 2];
                continue;
            }
            const int ia = 255 - a;
            dst[x * 3 + 0] = static_cast<std::uint8_t>((src[x * 4 + 0] * a + dst[x * 3 + 0] * ia) / 255);
            dst[x * 3 + 1] = static_cast<std::uint8_t>((src[x * 4 + 1] * a + dst[x * 3 + 1] * ia) / 255);
            dst[x * 3 + 2] = static_cast<std::uint8_t>((src[x * 4 + 2] * a + dst[x * 3 + 2] * ia) / 255);
        }
    }
}

int contentTop(const BrowseShellLayout& L) { return L.headerH + 4; }

void drawHeader(std::uint8_t* rgb, int w, int h, const plexui::FontSet& font, const BrowseShellLayout& L,
                std::string_view title, std::string_view right) {
    if (clip.y0 > L.headerH + L.fontLg)
        return;
    const int rightW = right.empty() ? 0 : textW(font, right, L.fontSm, false) + L.margin;
    text(font, L.margin, L.margin / 2 + 2,
         ellipsize(font, title, L.fontLg, true, w - 2 * L.margin - rightW), L.fontLg, true, TEXT);
    if (!right.empty()) {
        const int tw = textW(font, right, L.fontSm, false);
        text(font, w - L.margin - tw, L.margin / 2 + 4, right, L.fontSm, false, DIM);
    }
    hline(rgb, w, h, L.margin, w - L.margin, L.headerH, RULE);
}

void drawFooter(std::uint8_t* rgb, int w, int h, const plexui::FontSet& font, const BrowseShellLayout& L,
                std::string_view s) {
    const int y = h - L.footerH;
    if (clip.y1 <= y)
        return;
    hline(rgb, w, h, L.margin, w - L.margin, y, RULE);
    text(font, L.margin, y + 4, s, L.fontSm, false, DIM);
}

void focusRow(std::uint8_t* rgb, int w, int h, int x, int y, int rw, int rh) {
    fillRect(rgb, w, h, x, y, x + rw, y + rh, FOCUS_BAR);
    fillRect(rgb, w, h, x, y, x + 3, y + rh, AMBER);
}

PaintText monogram(std::string_view title) {
    char parts[2];
    std::size_t n = 0;
    char prev = ' ';
    for (char c : title) {
        if (c == ':')
            c = ' ';
        if ((prev == ' ' || prev == '-') && std::isalnum(static_cast<unsigned char>(c)))
            parts[n++] = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        prev = c;
        if (n >= 2)
            break;
    }
    return n < 2 ? PaintText(title.substr(0, 2)) : PaintText(std::string_view(parts, n));
}

void drawPoster(std::uint8_t* rgb, int w, int h, const plexui::FontSet& font, int x, int y, int tw, int th,
                std::string_view title, int colorIdx, bool focused, int ring, const BrowseThumb* thumb) {
    const auto& col = kPosterColors[static_cast<std::size_t>(colorIdx) % (sizeof kPosterColors / sizeof kPosterColors[0])];
    if (thumb && static_cast<int>(thumb->rgb.size()) >= tw * th * 3 && thumb->w == tw && thumb->h == th)
        blitRgb(rgb, w, h, x, y, tw, th, thumb->rgb.data());
    else {
        fillRect(rgb, w, h, x, y, x + tw, y + th, col);
        const auto mono = monogram(title);
        const int fs = std::max(10, tw / 4);
        const int mw = textW(font, mono, fs, true);
        text(font, x + (tw - mw) / 2, y + th / 2 - fs / 2, mono, fs, true, TEXT);
    }
    if (focused)
        strokeRect(rgb, w, h, x, y, x + tw, y + th, AMBER, ring);
    else
        strokeRect(rgb, w, h, x, y, x + tw, y + th, RULE, 1);
}

void cachedPoster(std::uint8_t* rgb, int w, int h, const plexui::FontSet& font,
                  int x, int y, int tw, int th, const BrowseTitleRow& t, int colorIdx,
                  bool focused, int ring, const BrowsePaintThumbs* thumbs) {
    if (x + tw + ring <= clip.x0 || x - ring >= clip.x1 ||
        y + th + ring <= clip.y0 || y - ring >= clip.y1)
        return;
    if (thumbs && thumbs->ram && !t.ratingKey.empty() &&
        thumbs->ram->paint(thumbs->serverId, t.ratingKey, tw, th, [&](const BrowseThumb& thumb) {
            drawPoster(rgb, w, h, font, x, y, tw, th, t.title, colorIdx, focused, ring, &thumb);
        }))
        return;
    drawPoster(rgb, w, h, font, x, y, tw, th, t.title, colorIdx, focused, ring, nullptr);
}

void paintServers(std::uint8_t* rgb, int w, int h, const plexui::FontSet& font, const BrowseShellLayout& L,
                  const BrowseRenderModel& m) {
    drawHeader(rgb, w, h, font, L, fallback(m.header, "SERVERS"), fallback(m.headerRight, "[Search]"));
    const int rw = w - 2 * L.margin;
    const int n = std::min(static_cast<int>(m.servers.size()), L.listRows);
    const int start = m.scroll;
    for (int i = 0; i < n && start + i < static_cast<int>(m.servers.size()); ++i) {
        const auto& s = m.servers[static_cast<std::size_t>(start + i)];
        const int y = L.rowY[static_cast<std::size_t>(i)];
        if (!overlaps(L.margin, y, rw, std::max(L.rowH, L.fontMd + 4)))
            continue;
        if (start + i == m.focus)
            focusRow(rgb, w, h, L.margin, y, rw, L.rowH);
        const int nameW = L.compact ? rw * 2 / 3 : rw / 2 - 16;
        text(font, L.margin + 8, y + 3, ellipsize(font, s.name, L.fontMd, false, nameW),
             L.fontMd, false, TEXT);
        const auto* stc = s.status == "OK" ? OKC : (s.status == "—" ? ERRORC : DIM);
        const int sw = textW(font, s.status, L.fontSm, false);
        text(font, w - L.margin - sw - 4, y + 4, s.status, L.fontSm, false, stc);
        PaintText mid(s.locality);
        if (!L.compact) {
            mid += "  ";
            mid += s.host;
        }
        if (L.compact) {
            text(font, L.margin + nameW + 12, y + 4, mid, L.fontSm, false, DIM);
        } else {
            text(font, L.margin + rw / 2, y + 4, mid, L.fontSm, false, DIM);
        }
    }
    drawFooter(rgb, w, h, font, L,
               fallback(m.footer, L.compact ? "PgUp/Dn  A select  B back" :
                                              "PgUp/PgDn scroll · A select · B back"));
}

void paintLibraries(std::uint8_t* rgb, int w, int h, const plexui::FontSet& font, const BrowseShellLayout& L,
                    const BrowseRenderModel& m) {
    drawHeader(rgb, w, h, font, L, fallback(m.header, "LIBRARIES"),
               m.headerRight.empty() ? m.serverName : m.headerRight);
    const int rw = w - 2 * L.margin;
    const int start = m.scroll;
    for (int i = 0; i < L.listRows && start + i < static_cast<int>(m.libraries.size()); ++i) {
        const auto& lib = m.libraries[static_cast<std::size_t>(start + i)];
        const int y = L.rowY[static_cast<std::size_t>(i)];
        if (!overlaps(L.margin, y, rw, std::max(L.rowH, L.fontMd + 4)))
            continue;
        if (start + i == m.focus)
            focusRow(rgb, w, h, L.margin, y, rw, L.rowH);
        PaintText label(lib.type == "show" ? "TV  " : "MOV  ");
        label += lib.title;
        text(font, L.margin + 8, y + 3, label, L.fontMd, false, TEXT);
        PaintText right(lib.type);
        if (lib.count > 0) {
            right = number(lib.count);
            right += " titles";
        }
        const int rwgt = textW(font, right, L.fontSm, false);
        text(font, w - L.margin - rwgt - 4, y + 4, right, L.fontSm, false, DIM);
    }
    drawFooter(rgb, w, h, font, L, fallback(m.footer, L.compact ? "PgUp/Dn  A open  B servers" :
                                                              "PgUp/PgDn · A open · B servers"));
}

void paintTitlesList(std::uint8_t* rgb, int w, int h, const plexui::FontSet& font, const BrowseShellLayout& L,
                     const BrowseRenderModel& m, const BrowsePaintThumbs* thumbs) {
    PaintText hdr(m.header.empty() ? m.libraryName : m.header);
    if (m.header.empty())
        hdr += " · List";
    PaintText right(m.headerRight);
    if (m.headerRight.empty()) {
        right = number(m.totalTitles);
        right += " titles";
    }
    drawHeader(rgb, w, h, font, L, hdr, right);
    const int top = contentTop(L);
    int listRight = w - L.margin;
    if (L.posterW > 0 && !m.titles.empty()) {
        listRight = w - L.margin - L.posterW - L.margin / 2;
        const int i = browseClampIndex(m.focus, static_cast<int>(m.titles.size()));
        const auto& t = m.titles[static_cast<std::size_t>(i)];
        const int px0 = listRight + L.margin / 2;
        const int py = top + 4;
        cachedPoster(rgb, w, h, font, px0, py, L.posterW, L.posterH, t, i, false, L.focusRing, thumbs);
        text(font, px0, py + L.posterH + 6, ellipsize(font, t.title, L.fontSm, false, L.posterW), L.fontSm,
             false, TEXT);
        text(font, px0, py + L.posterH + 6 + L.fontSm + 2, t.year, L.fontSm, false, DIM);
    }
    const int rw = listRight - L.margin;
    const int start = m.scroll;
    for (int i = 0; i < L.listRows && start + i < static_cast<int>(m.titles.size()); ++i) {
        const auto& t = m.titles[static_cast<std::size_t>(start + i)];
        const int y = L.rowY[static_cast<std::size_t>(i)];
        if (!overlaps(L.margin, y, rw, std::max(L.rowH, L.fontMd + 4)))
            continue;
        if (start + i == m.focus)
            focusRow(rgb, w, h, L.margin, y, rw, L.rowH);
        const char* mark = t.viewCount > 0 ? "*" : (t.progress > 0.f ? ">" : " ");
        PaintText label(mark);
        label += " ";
        label += t.title;
        PaintText meta(t.year);
        if (!L.compact && t.runtimeMs > 0) {
            meta += "  ";
            meta += number(t.runtimeMs / 60000);
            meta += "m";
        }
        const int yw = textW(font, meta, L.fontSm, false);
        const int maxW = rw - yw - 20;
        label = ellipsize(font, label, L.fontMd, false, maxW);
        text(font, L.margin + 6, y + 3, label, L.fontMd, false, TEXT);
        text(font, listRight - yw - 6, y + 4, meta, L.fontSm, false, DIM);
        if (t.progress > 0.f && !L.compact) {
            const int bx = L.margin + 6;
            const int by = y + L.rowH - 4;
            const int bw = rw * 35 / 100;
            fillRect(rgb, w, h, bx, by, bx + bw, by + 2, RULE);
            fillRect(rgb, w, h, bx, by, bx + static_cast<int>(bw * t.progress), by + 2, AMBER);
        }
    }
    drawFooter(rgb, w, h, font, L,
               fallback(m.footer, L.compact ? "PgUp/Dn A play Y grid B back" :
                                              "PgUp/PgDn · A play · Y grid · B libraries"));
}

void paintTitlesGrid(std::uint8_t* rgb, int w, int h, const plexui::FontSet& font, const BrowseShellLayout& L,
                     const BrowseRenderModel& m, const BrowsePaintThumbs* thumbs) {
    PaintText hdr(m.header.empty() ? m.libraryName : m.header);
    if (m.header.empty())
        hdr += " · Grid";
    PaintText right(m.headerRight);
    if (m.headerRight.empty()) {
        right = number(m.totalTitles);
        right += " titles";
    }
    drawHeader(rgb, w, h, font, L, hdr, right);
    const int cols = L.gridCols;
    const int rows = L.gridRows;
    const int tw = L.tileW, th = L.tileH;
    const int start = m.scroll;
    std::string_view focusTitle;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int i = start + r * cols + c;
            if (i >= static_cast<int>(m.titles.size()))
                break;
            const auto& t = m.titles[static_cast<std::size_t>(i)];
            const auto tile = static_cast<std::size_t>(r * cols + c);
            const int x = L.tileX[tile];
            const int y = L.tileY[tile];
            const bool focused = i == m.focus;
            if (focused)
                focusTitle = t.title;
            if (!overlaps(x - L.focusRing, y - L.focusRing, tw + 2 * L.focusRing,
                          th + (L.titleUnder ? L.fontSm + 5 : 0) + 2 * L.focusRing))
                continue;
            cachedPoster(rgb, w, h, font, x, y, tw, th, t, i, focused, L.focusRing, thumbs);
            if (L.titleUnder) {
                const auto shortT = ellipsize(font, t.title, L.fontSm, false, tw);
                const int twt = textW(font, shortT, L.fontSm, false);
                text(font, x + (tw - twt) / 2, y + th + 3, shortT, L.fontSm, false, focused ? TEXT : DIM);
            }
        }
    }
    if (!L.titleUnder) {
        const std::string_view controls = "  A play Y list";
        auto foot = ellipsize(font, focusTitle, L.fontSm, false,
                                     w - 2 * L.margin - textW(font, controls, L.fontSm, false));
        foot += controls;
        drawFooter(rgb, w, h, font, L, m.footer.empty() ? std::string_view(foot) : m.footer);
    } else {
        drawFooter(rgb, w, h, font, L,
                   fallback(m.footer, "PgUp/PgDn · A play · Y list · B libraries"));
    }
}

void paintDetail(std::uint8_t* rgb, int w, int h, const plexui::FontSet& font, const BrowseShellLayout& L,
                 const BrowseRenderModel& m, const BrowsePaintThumbs* thumbs) {
    drawHeader(rgb, w, h, font, L, fallback(m.header, "DETAIL"),
               m.headerRight.empty() ? m.serverName : m.headerRight);
    const auto& t = m.detail.item;
    const int top = contentTop(L) + (L.compact ? 4 : 10);
    int textX = L.margin;
    if (L.posterW > 0) {
        const int pw = L.posterW * 2 / 3;
        const int ph = L.posterH * 2 / 3;
        cachedPoster(rgb, w, h, font, L.margin, top, pw, ph, t, 0, false, 1, thumbs);
        textX = L.margin + pw + L.margin;
    }
    text(font, textX, top, ellipsize(font, t.title, L.fontXl, true, w - textX - L.margin),
         L.fontXl, true, TEXT);
    PaintText meta(t.year);
    if (!t.type.empty()) {
        meta += "  ·  ";
        meta += t.type;
    }
    if (t.runtimeMs > 0) {
        const int mins = t.runtimeMs / 60000;
        meta += "  ·  ";
        meta += number(mins / 60);
        meta += "h ";
        meta += number(mins % 60);
        meta += "m";
    }
    text(font, textX, top + L.fontXl + 4, meta, L.fontSm, false, DIM);
    const int maxW = w - textX - L.margin;
    const int maxLines = L.compact ? 2 : 3;
    std::string_view remaining(t.summary);
    for (int line = 0; line < maxLines && !remaining.empty(); ++line) {
        std::size_t end = font.fittingPrefix(remaining, static_cast<float>(L.fontMd), false,
                                            static_cast<float>(maxW));
        const auto space = remaining.substr(0, end).find_last_of(' ');
        const auto word = space == std::string_view::npos ? 0 : space;
        if (end < remaining.size() && word && remaining[end] != '\n')
            end = word;
        if (line == maxLines - 1) {
            text(font, textX, top + L.fontXl + L.fontSm + 14 + line * (L.fontMd + 4),
                 ellipsize(font, remaining, L.fontMd, false, maxW), L.fontMd, false, TEXT);
            break;
        }
        if (!end)
            break;
        text(font, textX, top + L.fontXl + L.fontSm + 14 + line * (L.fontMd + 4),
             remaining.substr(0, end), L.fontMd, false, TEXT);
        remaining.remove_prefix(end);
        while (!remaining.empty() && (remaining.front() == ' ' || remaining.front() == '\n'))
            remaining.remove_prefix(1);
    }
    if (m.detail.inProgress || t.progress > 0.f) {
        const int py = top + L.fontXl + L.fontSm + 14 + maxLines * (L.fontMd + 4) + 8;
        const int pct = static_cast<int>((t.progress > 0.f ? t.progress : 0.f) * 100);
        PaintText progress("In progress · ");
        progress += number(pct);
        progress += "%";
        text(font, textX, py, progress, L.fontSm, false, AMBER);
        const int bx = textX;
        const int by = py + L.fontSm + 4;
        const int bw = std::min(220, maxW);
        fillRect(rgb, w, h, bx, by, bx + bw, by + 4, RULE);
        fillRect(rgb, w, h, bx, by, bx + static_cast<int>(bw * t.progress), by + 4, AMBER);
    }
    const int btnY = h - L.footerH - L.btnH - (L.compact ? 4 : 10);
    struct Btn {
        const char* label;
        bool on;
    };
    Btn buttons[3];
    int nBtn = 0;
    if (m.detail.inProgress) {
        buttons[nBtn++] = {"Resume", m.detail.button == 0};
        buttons[nBtn++] = {"From start", m.detail.button == 1};
    } else {
        buttons[nBtn++] = {"Play", true};
    }
    int bx = L.posterW > 0 ? textX : L.margin;
    const int gap = L.compact ? 6 : 10;
    for (int i = 0; i < nBtn; ++i) {
        const int bw = L.compact ? 70 : L.btnW;
        if (buttons[i].on)
            fillRect(rgb, w, h, bx, btnY, bx + bw, btnY + L.btnH, AMBER);
        else {
            fillRect(rgb, w, h, bx, btnY, bx + bw, btnY + L.btnH, PANEL);
            strokeRect(rgb, w, h, bx, btnY, bx + bw, btnY + L.btnH, RULE, 1);
        }
        const auto* tc = buttons[i].on ? BG_BOT : TEXT;
        const int tw = textW(font, buttons[i].label, L.fontMd, buttons[i].on);
        text(font, bx + (bw - tw) / 2, btnY + (L.btnH - L.fontMd) / 2, buttons[i].label, L.fontMd,
             buttons[i].on, tc);
        bx += bw + gap;
    }
    drawFooter(rgb, w, h, font, L, fallback(m.footer, "A select · B back to titles"));
}

void paintSearch(std::uint8_t* rgb, int w, int h, const plexui::FontSet& font, const BrowseShellLayout& L,
                 const BrowseRenderModel& m) {
    drawHeader(rgb, w, h, font, L, fallback(m.header, "SEARCH"),
               m.headerRight.empty() ? m.search.scopeLabel : m.headerRight);
    const int top = contentTop(L);
    const auto& s = m.search;
    const int fx = L.margin, fy = top + 4;
    const int fw = w - 2 * L.margin;
    const int fh = L.rowH + (L.compact ? 2 : 4);
    fillRect(rgb, w, h, fx, fy, fx + fw, fy + fh, PANEL);
    strokeRect(rgb, w, h, fx, fy, fx + fw, fy + fh, AMBER, 1);
    PaintText q(s.query);
    if (s.region == BrowseSearchUi::Region::Field)
        q += "|";
    text(font, fx + 8, fy + 3, q, L.fontMd, false, TEXT);
    const int cell = L.kbCell;
    const int nKeys = static_cast<int>(s.keys.size());
    const int resultsY = L.searchResultsY;
    {
        for (int i = 0; i < nKeys; ++i) {
            const int cx = L.keyX[static_cast<std::size_t>(i)];
            const int cy = L.keyY[static_cast<std::size_t>(i)];
            if (!overlaps(cx - 1, cy - 1, cell + 2, cell + 2))
                continue;
            const bool on = s.region == BrowseSearchUi::Region::Keyboard && i == s.kbIndex;
            if (on)
                fillRect(rgb, w, h, cx, cy, cx + cell, cy + cell, AMBER);
            else {
                fillRect(rgb, w, h, cx, cy, cx + cell, cy + cell, PANEL);
                strokeRect(rgb, w, h, cx, cy, cx + cell, cy + cell, RULE, 1);
            }
            const auto* tc = on ? BG_BOT : TEXT;
            const int tw = textW(font, s.keys[static_cast<std::size_t>(i)].label, std::max(8, L.fontSm - 1), on);
            text(font, cx + (cell - tw) / 2, cy + 2, s.keys[static_cast<std::size_t>(i)].label,
                 std::max(8, L.fontSm - 1), on, tc);
        }
    }
    const int rw = w - 2 * L.margin;
    const int maxR = L.compact ? 3 : 4;
    for (int i = 0; i < maxR && s.resultScroll + i < static_cast<int>(s.results.size()); ++i) {
        const auto& t = s.results[static_cast<std::size_t>(s.resultScroll + i)];
        const int y = resultsY + i * L.rowH;
        if (s.region == BrowseSearchUi::Region::Results && s.resultScroll + i == s.resultFocus)
            focusRow(rgb, w, h, L.margin, y, rw, L.rowH);
        text(font, L.margin + 8, y + 3, ellipsize(font, t.title, L.fontMd, false, rw - 70),
             L.fontMd, false, TEXT);
        const int yw = textW(font, t.year, L.fontSm, false);
        text(font, w - L.margin - yw - 4, y + 4, t.year, L.fontSm, false, DIM);
    }
    drawFooter(rgb, w, h, font, L, fallback(m.footer, "Type · A add · B back"));
}

void paintLoading(std::uint8_t* rgb, int w, int h, const plexui::FontSet& font, const BrowseShellLayout& L,
                  const BrowseRenderModel& m) {
    PaintText msg("Loading… ");
    msg += number(std::max(0, std::min(100, m.loadingPct)));
    msg += "%";
    const int fs = L.fontXl;
    const int tw = textW(font, msg, fs, true);
    text(font, (w - tw) / 2, h / 2 - fs, msg, fs, true, TEXT);
    const int bw = std::min(w - 2 * L.margin, L.compact ? 200 : 360);
    const int bh = L.compact ? 8 : 12;
    const int bx = (w - bw) / 2;
    const int by = h / 2 + fs;
    fillRect(rgb, w, h, bx, by, bx + bw, by + bh, RULE);
    fillRect(rgb, w, h, bx, by, bx + bw * std::max(0, std::min(100, m.loadingPct)) / 100, by + bh, AMBER);
    if (!m.status.empty())
        text(font, L.margin, h - L.footerH, m.status, L.fontSm, false, DIM);
}

} // namespace

void paintBrowseShell(const BrowseRenderModel& m, const BrowseShellLayout& L, const plexui::FontSet& font,
                      std::uint8_t* rgb, int w, int h, const BrowsePaintThumbs* thumbs,
                      const BrowseRect* dirty) {
    if (!rgb || w <= 0 || h <= 0)
        return;
    clip = dirty ? *dirty : BrowseRect{0, 0, w, h};
    clip.x0 = std::max(0, clip.x0);
    clip.y0 = std::max(0, clip.y0);
    clip.x1 = std::min(w, clip.x1);
    clip.y1 = std::min(h, clip.y1);
    if (clip.x0 >= clip.x1 || clip.y0 >= clip.y1)
        return;
    fillBg(rgb, w, h);
    g_rgba.ensure(w, h);
    g_rgba.clear();
    switch (m.screen) {
    case BrowseShellScreen::Servers:
        paintServers(rgb, w, h, font, L, m);
        break;
    case BrowseShellScreen::Libraries:
        paintLibraries(rgb, w, h, font, L, m);
        break;
    case BrowseShellScreen::Titles:
        if (m.view == BrowseViewMode::Grid)
            paintTitlesGrid(rgb, w, h, font, L, m, thumbs);
        else
            paintTitlesList(rgb, w, h, font, L, m, thumbs);
        break;
    case BrowseShellScreen::Detail:
        paintDetail(rgb, w, h, font, L, m, thumbs);
        break;
    case BrowseShellScreen::Search:
        paintSearch(rgb, w, h, font, L, m);
        break;
    case BrowseShellScreen::Loading:
        paintLoading(rgb, w, h, font, L, m);
        break;
    }
    if (!m.status.empty() && m.screen != BrowseShellScreen::Loading)
        text(font, L.margin, h - L.footerH - L.fontSm - 2,
             ellipsize(font, m.status, L.fontSm, false, w - 2 * L.margin), L.fontSm, false, AMBER);
    compositeText(rgb, w, h);
}

void seedBrowseDemoModel(BrowseRenderModel& m, BrowseShellScreen screen, const BrowseShellLayout& L) {
    m = {};
    m.screen = screen;
    m.w = L.w;
    m.h = L.h;
    m.view = browseDefaultView(L);
    m.serverName = "Living Room PMS";
    m.libraryName = "MOVIES";
    m.totalTitles = 128;
    m.servers = {
        {"id1", "Living Room PMS", "local", "192.0.2.20", "OK", "http://192.0.2.20:32400", "", true, true, {}},
        {"id2", "Cabin NAS", "remote", "relay", "OK", "", "", true, false, {}},
        {"id3", "Office", "local", "timed out", "—", "", "", false, false, {}},
        {"id4", "Basement Pi", "local", "192.0.2.44", "OK", "", "", true, false, {}},
        {"id5", "Travel Laptop", "remote", "relay", "OK", "", "", true, false, {}},
    };
    m.libraries = {
        {"1", "Movies", "movie", 128}, {"2", "TV Shows", "show", 42}, {"3", "Kids Movies", "movie", 31},
        {"4", "Documentaries", "movie", 57}, {"5", "Anime", "show", 18},
    };
    const char* titles[][2] = {
        {"Blade Runner 2049", "2017"}, {"Arrival", "2016"},       {"Dune", "2021"},
        {"The Matrix", "1999"},        {"Interstellar", "2014"},  {"Ex Machina", "2014"},
        {"Mad Max: Fury Road", "2015"},{"Alien", "1979"},         {"The Thing", "1982"},
        {"Children of Men", "2006"},   {"Ghost in the Shell", "1995"}, {"Akira", "1988"},
        {"Princess Mononoke", "1997"}, {"Spirited Away", "2001"}, {"The Iron Giant", "1999"},
        {"Wall-E", "2008"},            {"Inception", "2010"},     {"Tenet", "2020"},
        {"Heat", "1995"},              {"Drive", "2011"},         {"No Country for Old Men", "2007"},
        {"Sicario", "2015"},           {"Whiplash", "2014"},      {"Her", "2013"},
    };
    const float prog[] = {0.62f, 0, 0.35f, 0, 0.88f, 0, 0.12f, 0, 0, 0.45f, 0, 0,
                          0.20f, 0, 0, 0.55f, 0, 0.08f, 0, 0.70f, 0, 0.25f, 0, 0.40f};
    const int watched[] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0};
    for (int i = 0; i < 24; ++i) {
        BrowseTitleRow t;
        t.ratingKey = std::to_string(i + 1);
        t.title = titles[i][0];
        t.year = titles[i][1];
        t.type = "Movie";
        t.progress = prog[i];
        t.viewOffsetMs = prog[i] > 0.f ? static_cast<int64_t>(prog[i] * 1000 * 60 * 60) : 0;
        t.viewCount = watched[i];
        t.runtimeMs = 164 * 60 * 1000;
        t.summary = "A young blade runner's discovery of a long-buried secret leads him to track down "
                    "former detective Rick Deckard, who's been missing for thirty years.";
        m.titles.push_back(std::move(t));
    }
    m.search.keys = browseSearchKeys(L);
    m.search.query = "blade";
    m.search.kbIndex = 3;
    m.search.region = BrowseSearchUi::Region::Keyboard;
    m.search.results = {m.titles[0], m.titles[0], m.titles[7], m.titles[1]};
    m.search.results[1].title = "Blade Runner";
    m.search.results[1].year = "1982";
    m.detail.item = m.titles[0];
    m.detail.inProgress = true;
    m.detail.button = 0;
    m.loadingPct = 42;
    m.header.clear();
    if (screen == BrowseShellScreen::Titles && m.view == BrowseViewMode::Grid)
        m.focus = 1;
    if (screen == BrowseShellScreen::Search)
        m.headerRight = "All libraries";
}

} // namespace misterplex
