#pragma once

#include "../../arm/misterplexd/font_8x16.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace misterplex {
namespace browse {

// R1 480p browse renderer constants. Keep these named values together so later
// UX tuning can adjust the locked palette/layout without hunting call sites.
inline constexpr int kCanvasW = 640;
inline constexpr int kCanvasH = 480;
inline constexpr int kStrideRgb24 = kCanvasW * 3;
inline constexpr int kTitleSafeX0 = 32;
inline constexpr int kTitleSafeX1 = 607;
inline constexpr int kTitleSafeY0 = 24;
inline constexpr int kTitleSafeY1 = 455;
inline constexpr int kActionSafeX0 = 16;
inline constexpr int kActionSafeX1 = 623;
inline constexpr int kActionSafeY0 = 12;
inline constexpr int kActionSafeY1 = 467;
inline constexpr int kHeaderY0 = 24;
inline constexpr int kHeaderY1 = 63;
inline constexpr int kHeaderRuleY0 = 62;
inline constexpr int kHeaderRuleY1 = 64;
inline constexpr int kListY0 = 72;
inline constexpr int kFooterY0 = 432;
inline constexpr int kFooterTextY = 436;
inline constexpr int kRowH = 24;
inline constexpr int kViewRows = 15;
inline constexpr int kGlyphW = 8;
inline constexpr int kGlyphH = 16;
inline constexpr int kTextX = 48;
inline constexpr int kMarkX = 32;
inline constexpr int kColumns = 70;
inline constexpr int kPosterColumns = 48;
inline constexpr int kHeaderTitleColumns2x = 30;
inline constexpr int kSelectionX0 = 24;
inline constexpr int kSelectionX1 = 615;
inline constexpr int kSelectionCoreX0 = 28;
inline constexpr int kSelectionCoreX1 = 611;
inline constexpr int kProgressBarX0 = 544;
inline constexpr int kProgressBarX1 = 607;
inline constexpr int kProgressTextRightX = 607;
inline constexpr int kPosterX0 = 448;
inline constexpr int kPosterY0 = 72;
inline constexpr int kPosterW = 160;
inline constexpr int kPosterH = 240;
inline constexpr std::uint8_t kBgTop[3] = {0x12, 0x16, 0x1C};
inline constexpr std::uint8_t kBgBottom[3] = {0x0B, 0x0E, 0x12};
inline constexpr std::uint8_t kText[3] = {0xE8, 0xE8, 0xE8};
inline constexpr std::uint8_t kDim[3] = {0x9A, 0xA0, 0xA6};
inline constexpr std::uint8_t kAmber[3] = {0xE5, 0xA0, 0x0D};
inline constexpr std::uint8_t kInk[3] = {0x10, 0x14, 0x18};
inline constexpr std::uint8_t kRule[3] = {0x2A, 0x30, 0x38};
inline constexpr std::uint8_t kError[3] = {0xFF, 0x6B, 0x5B};
inline constexpr std::uint8_t kPosterBg[3] = {0x1C, 0x21, 0x28};
inline constexpr std::size_t kPosterBlobBytes = 160u * 240u * 3u;

enum class BrowseState {
    S2Servers,
    S3Root,
    S4List,
    S5Detail,
    S6Starting,
    S8Error,
};

enum class RowKind {
    Directory,
    Item,
    Info,
    PagePrev,
    PageNext,
};

struct Row {
    RowKind kind = RowKind::Item;
    std::string label;
    int pct = -1;
    bool watched = false;
};

struct RenderModel {
    BrowseState state = BrowseState::S2Servers;
    std::string headerTitle;
    std::string pageIndicator;
    std::vector<Row> rows;
    int cursorIndex = 0;
    int viewportTop = 0;
    std::string footerText;
    bool error = false;       // S8: the whole page in the error colour
    bool footerError = false; // a status-line error on a normal page: only the footer in the error colour
    bool posterPanel = false;
    const std::uint8_t* posterBlob = nullptr;
    std::size_t posterBlobSize = 0;
    std::string posterMeta;
};

inline bool selectable(RowKind kind) {
    return kind != RowKind::Info;
}

inline std::uint8_t gradientComponent(std::uint8_t top, std::uint8_t bottom, int y) {
    const int delta = static_cast<int>(bottom) - static_cast<int>(top);
    const int den = kCanvasH - 1;
    const int num = delta * y;
    const int rounded = num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den);
    const int v = static_cast<int>(top) + rounded;
    return static_cast<std::uint8_t>(std::max(0, std::min(255, v)));
}

inline void backgroundForLine(int y, std::uint8_t rgb[3]) {
    rgb[0] = gradientComponent(kBgTop[0], kBgBottom[0], y);
    rgb[1] = gradientComponent(kBgTop[1], kBgBottom[1], y);
    rgb[2] = gradientComponent(kBgTop[2], kBgBottom[2], y);
}

inline void putPixel(std::uint8_t* line, int x, const std::uint8_t rgb[3]) {
    if (x < 0 || x >= kCanvasW)
        return;
    line[x * 3 + 0] = rgb[0];
    line[x * 3 + 1] = rgb[1];
    line[x * 3 + 2] = rgb[2];
}

inline void fillRectOnLine(std::uint8_t* line, int y, int x0, int y0, int x1, int y1,
                           const std::uint8_t rgb[3]) {
    if (y < y0 || y >= y1)
        return;
    x0 = std::max(0, x0);
    x1 = std::min(kCanvasW, x1);
    for (int x = x0; x < x1; ++x)
        putPixel(line, x, rgb);
}

inline char safeChar(unsigned char c, bool helperText) {
    if (c < 0x20 || c > 0x7E)
        return '?';
    if (helperText && (c == '<' || c == '>' || c == '"' || c == '&'))
        return '?';
    return static_cast<char>(c);
}

inline std::string sanitizeText(const std::string& in, bool helperText = true) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in)
        out.push_back(safeChar(c, helperText));
    return out;
}

inline std::string clipWithEllipsis(const std::string& in, int columns) {
    if (columns <= 0)
        return {};
    if (static_cast<int>(in.size()) <= columns)
        return in;
    if (columns <= 3)
        return in.substr(0, static_cast<std::size_t>(columns));
    return in.substr(0, static_cast<std::size_t>(columns - 3)) + "...";
}

inline int normalizedCursor(const RenderModel& model) {
    if (model.rows.empty())
        return 0;
    int c = std::max(0, std::min(static_cast<int>(model.rows.size()) - 1, model.cursorIndex));
    if (selectable(model.rows[static_cast<std::size_t>(c)].kind))
        return c;
    for (int i = c + 1; i < static_cast<int>(model.rows.size()); ++i) {
        if (selectable(model.rows[static_cast<std::size_t>(i)].kind))
            return i;
    }
    for (int i = c - 1; i >= 0; --i) {
        if (selectable(model.rows[static_cast<std::size_t>(i)].kind))
            return i;
    }
    return c;
}

inline int normalizedViewportTop(const RenderModel& model, int cursor) {
    const int count = static_cast<int>(model.rows.size());
    const int maxTop = std::max(0, count - kViewRows);
    int top = std::max(0, std::min(maxTop, model.viewportTop));
    if (cursor < top + 2)
        top = cursor - 2;
    if (cursor > top + 12)
        top = cursor - 12;
    return std::max(0, std::min(maxTop, top));
}

inline void drawGlyph(std::uint8_t* line, int lineY, int x, int y, char ch,
                      const std::uint8_t fg[3], int scale = 1) {
    unsigned char uc = static_cast<unsigned char>(ch);
    if (uc < 0x20 || uc > 0x7E)
        uc = '?';
    const auto& glyph = kFont8x16[static_cast<std::size_t>(uc - 0x20)];
    for (int gy = 0; gy < kGlyphH; ++gy) {
        for (int sy = 0; sy < scale; ++sy) {
            if (lineY != y + gy * scale + sy)
                continue;
            const std::uint8_t bits = glyph[static_cast<std::size_t>(gy)];
            for (int gx = 0; gx < kGlyphW; ++gx) {
                if (!(bits & (0x80u >> gx)))
                    continue;
                for (int sx = 0; sx < scale; ++sx)
                    putPixel(line, x + gx * scale + sx, fg);
            }
        }
    }
}

inline void drawText(std::uint8_t* line, int lineY, int x, int y, const std::string& text,
                     const std::uint8_t fg[3], int scale = 1) {
    for (char ch : text) {
        drawGlyph(line, lineY, x, y, ch, fg, scale);
        x += kGlyphW * scale;
    }
}

inline std::string rowLabel(const Row& row, int columns) {
    std::string label = sanitizeText(row.label);
    if (row.kind == RowKind::PagePrev)
        label = "< " + label;
    else if (row.kind == RowKind::PageNext)
        label += " >";
    return clipWithEllipsis(label, columns);
}

inline void drawTriangle(std::uint8_t* line, int lineY, int x, int y, bool up,
                         const std::uint8_t rgb[3]) {
    const int i = lineY - y;
    if (i < 0 || i >= 8)
        return;
    const int w = up ? i : 7 - i;
    const int x0 = x + 4 - (w + 1) / 2;
    const int x1 = x + 4 + (w + 1) / 2 + (w == 0 ? 1 : 0);
    for (int px = x0; px < x1; ++px)
        putPixel(line, px, rgb);
}

inline void drawSelectionBar(std::uint8_t* line, int y, int rowY) {
    fillRectOnLine(line, y, kSelectionCoreX0, rowY, kSelectionCoreX1 + 1, rowY + kRowH, kAmber);
    fillRectOnLine(line, y, kSelectionX0, rowY + 2, kSelectionCoreX0, rowY + kRowH - 2, kAmber);
    fillRectOnLine(line, y, kSelectionCoreX1 + 1, rowY + 2, kSelectionX1 + 1, rowY + kRowH - 2, kAmber);
}

inline void drawPosterPlaceholder(std::uint8_t* line, int y, const RenderModel& model) {
    fillRectOnLine(line, y, kPosterX0, kPosterY0, kPosterX0 + kPosterW, kPosterY0 + kPosterH, kPosterBg);
    fillRectOnLine(line, y, kPosterX0, kPosterY0, kPosterX0 + kPosterW, kPosterY0 + 2, kRule);
    fillRectOnLine(line, y, kPosterX0, kPosterY0 + kPosterH - 2, kPosterX0 + kPosterW, kPosterY0 + kPosterH, kRule);
    fillRectOnLine(line, y, kPosterX0, kPosterY0, kPosterX0 + 2, kPosterY0 + kPosterH, kRule);
    fillRectOnLine(line, y, kPosterX0 + kPosterW - 2, kPosterY0, kPosterX0 + kPosterW, kPosterY0 + kPosterH, kRule);
    if (model.posterBlob && model.posterBlobSize == kPosterBlobBytes && y >= kPosterY0 && y < kPosterY0 + kPosterH) {
        const std::uint8_t* src = model.posterBlob + static_cast<std::size_t>(y - kPosterY0) * kPosterW * 3u;
        std::memcpy(line + kPosterX0 * 3, src, kPosterW * 3u);
    } else {
        std::string initials;
        for (std::size_t i = 0; i < model.headerTitle.size() && initials.size() < 2; ++i) {
            const unsigned char c = static_cast<unsigned char>(model.headerTitle[i]);
            if ((i == 0 || model.headerTitle[i - 1] == ' ') && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
                initials.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c));
        }
        if (initials.empty())
            initials = "?";
        const int x = kPosterX0 + (kPosterW - static_cast<int>(initials.size()) * kGlyphW * 2) / 2;
        drawText(line, y, x, kPosterY0 + 96, initials, kAmber, 2);
    }
    if (!model.posterMeta.empty()) {
        const std::string meta = clipWithEllipsis(sanitizeText(model.posterMeta), 20);
        const int x = kPosterX0 + (kPosterW - static_cast<int>(meta.size()) * kGlyphW) / 2;
        drawText(line, y, x, 320, meta, kDim);
    }
}

inline void renderBrowseLine(const RenderModel& model, int y, std::uint8_t* line) {
    if (!line || y < 0 || y >= kCanvasH)
        return;

    std::uint8_t bg[3]{};
    backgroundForLine(y, bg);
    for (int x = 0; x < kCanvasW; ++x)
        putPixel(line, x, bg);

    if (model.state == BrowseState::S6Starting) {
        const std::string msg = clipWithEllipsis(sanitizeText(model.footerText.empty() ? model.headerTitle : model.footerText), kColumns);
        const int x = (kCanvasW - static_cast<int>(msg.size()) * kGlyphW) / 2;
        drawText(line, y, x, 232, msg, model.error ? kError : kDim);
        return;
    }

    const std::string title = clipWithEllipsis(sanitizeText(model.headerTitle), kHeaderTitleColumns2x);
    drawText(line, y, kTitleSafeX0, kHeaderY0, title, model.error ? kError : kAmber, 2);
    if (!model.pageIndicator.empty()) {
        const std::string page = clipWithEllipsis(sanitizeText(model.pageIndicator), 24);
        drawText(line, y, kTitleSafeX1 - kGlyphW * static_cast<int>(page.size()) + 1, 40, page, kDim);
    }
    fillRectOnLine(line, y, kTitleSafeX0, kHeaderRuleY0, kTitleSafeX1 + 1, kHeaderRuleY1, kRule);

    const int cursor = normalizedCursor(model);
    const int top = normalizedViewportTop(model, cursor);
    const int columns = model.posterPanel ? kPosterColumns : kColumns;
    for (int vi = 0; vi < kViewRows; ++vi) {
        const int ri = top + vi;
        if (ri < 0 || ri >= static_cast<int>(model.rows.size()))
            continue;
        const Row& row = model.rows[static_cast<std::size_t>(ri)];
        const int rowY = kListY0 + vi * kRowH;
        const bool selected = (ri == cursor) && selectable(row.kind);
        if (selected)
            drawSelectionBar(line, y, rowY);
        const std::uint8_t* fg = selected ? kInk : (row.kind == RowKind::Info ? kDim : (model.error ? kError : kText));
        if (row.kind == RowKind::Directory)
            drawText(line, y, kMarkX, rowY + 4, ">", selected ? kInk : kDim);
        drawText(line, y, kTextX, rowY + 4, rowLabel(row, columns), fg);
        if (row.watched)
            drawText(line, y, 599, rowY + 4, "W", selected ? kInk : kDim);
        if (row.pct >= 0) {
            const int pct = std::max(0, std::min(100, row.pct));
            const std::string pctText = std::to_string(pct) + "%";
            drawText(line, y, kProgressTextRightX - kGlyphW * static_cast<int>(pctText.size()) + 1 - 72,
                     rowY + 4, pctText, selected ? kInk : kDim);
            const std::uint8_t selRule[3] = {0x9A, 0x6A, 0x08};
            fillRectOnLine(line, y, kProgressBarX0, rowY + 20, kProgressBarX1 + 1, rowY + 22,
                           selected ? selRule : kRule);
            const int width = (64 * pct + 50) / 100;
            fillRectOnLine(line, y, kProgressBarX0, rowY + 20, kProgressBarX0 + width, rowY + 22,
                           selected ? kInk : kAmber);
        }
    }

    if (model.posterPanel && (model.state == BrowseState::S4List || model.state == BrowseState::S5Detail))
        drawPosterPlaceholder(line, y, model);

    drawText(line, y, kTitleSafeX0, kFooterTextY,
             clipWithEllipsis(sanitizeText(model.footerText), kColumns),
             (model.error || model.footerError) ? kError : kDim);
    if (top > 0)
        drawTriangle(line, y, 590, 436, true, kDim);
    if (top + kViewRows < static_cast<int>(model.rows.size()))
        drawTriangle(line, y, 600, 444, false, kDim);
}

inline void renderBrowseRgb24(const RenderModel& model, std::uint8_t* rgb,
                              std::size_t stride = kStrideRgb24) {
    if (!rgb || stride < static_cast<std::size_t>(kStrideRgb24))
        return;
    for (int y = 0; y < kCanvasH; ++y)
        renderBrowseLine(model, y, rgb + static_cast<std::size_t>(y) * stride);
}

} // namespace browse
} // namespace misterplex
