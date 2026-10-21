#include "plexui_font.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"

namespace misterplex {
namespace plexui {

namespace {

bool readFile(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    if (n <= 0)
        return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(n));
    in.read(reinterpret_cast<char*>(out.data()), n);
    return static_cast<std::streamoff>(in.gcount()) == n;
}

int contByte(const std::string& text, size_t idx) {
    if (idx >= text.size())
        return -1;
    const unsigned char c = static_cast<unsigned char>(text[idx]);
    if ((c & 0xC0) != 0x80)
        return -1;
    return c & 0x3F;
}

// ASCII stand-in when the face has no glyph (Roboto covers the common set; this is the miss path).
int asciiFallback(int cp) {
    if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A || cp == 0x201B || cp == 0x2032 || cp == 0x00B4)
        return '\'';
    if (cp == 0x201C || cp == 0x201D || cp == 0x201E || cp == 0x201F || cp == 0x2033 || cp == 0x00AB ||
        cp == 0x00BB)
        return '"';
    if (cp == 0x2010 || cp == 0x2011 || cp == 0x2012 || cp == 0x2013 || cp == 0x2014 || cp == 0x2015 ||
        cp == 0x2212 || cp == 0x00AD)
        return '-';
    if (cp == 0x2026 || cp == 0x22EF)
        return '.';
    if (cp == 0x00A0 || cp == 0x202F || cp == 0x2007 || cp == 0x2009 || cp == 0x200A)
        return ' ';
    switch (cp) {
    case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5: return 'A';
    case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5: return 'a';
    case 0x00C7: return 'C';
    case 0x00E7: return 'c';
    case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB: return 'E';
    case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB: return 'e';
    case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF: return 'I';
    case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF: return 'i';
    case 0x00D1: return 'N';
    case 0x00F1: return 'n';
    case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6: return 'O';
    case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6: return 'o';
    case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC: return 'U';
    case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC: return 'u';
    case 0x00DD: return 'Y';
    case 0x00FD: case 0x00FF: return 'y';
    case 0x00DF: return 's';
    default: return 0;
    }
}

int utf8Codepoint(const std::string& text, size_t& i) {
    if (i >= text.size())
        return -1;
    const unsigned char c0 = static_cast<unsigned char>(text[i]);
    if (c0 < 0x80) {
        ++i;
        return c0;
    }
    if ((c0 & 0xE0) == 0xC0) {
        const int c1 = contByte(text, i + 1);
        if (c1 < 0) {
            ++i;
            return '?';
        }
        i += 2;
        const int cp = ((c0 & 0x1F) << 6) | c1;
        return cp >= 0x80 ? cp : '?';
    }
    if ((c0 & 0xF0) == 0xE0) {
        const int c1 = contByte(text, i + 1);
        const int c2 = contByte(text, i + 2);
        if (c1 < 0 || c2 < 0) {
            ++i;
            return '?';
        }
        i += 3;
        const int cp = ((c0 & 0x0F) << 12) | (c1 << 6) | c2;
        return cp >= 0x800 ? cp : '?';
    }
    if ((c0 & 0xF8) == 0xF0) {
        const int c1 = contByte(text, i + 1);
        const int c2 = contByte(text, i + 2);
        const int c3 = contByte(text, i + 3);
        if (c1 < 0 || c2 < 0 || c3 < 0) {
            ++i;
            return '?';
        }
        i += 4;
        const int cp = ((c0 & 0x07) << 18) | (c1 << 12) | (c2 << 6) | c3;
        return (cp >= 0x10000 && cp <= 0x10FFFF) ? cp : '?';
    }
    ++i;
    return '?';
}

} // namespace

bool FontSet::load(const std::string& dir, std::string& err) {
    std::string base = dir;
    if (!base.empty() && base.back() != '/')
        base += '/';
    if (!readFile(base + "Roboto-Regular.ttf", regularData_) ||
        !readFile(base + "Roboto-Bold.ttf", boldData_)) {
        if (!readFile(base + "../third_party/Roboto-Regular.ttf", regularData_) ||
            !readFile(base + "../third_party/Roboto-Bold.ttf", boldData_)) {
            err = "Roboto fonts missing in " + dir;
            return false;
        }
    }
    return true;
}

const std::vector<std::uint8_t>& FontSet::ttfData(bool bold) const {
    return bold ? boldData_ : regularData_;
}

FontSet::GlyphBmp FontSet::rasterGlyph(int cp, float size, bool bold) const {
    GlyphKey key{cp, size, bold};
    const auto it = cache_.find(key);
    if (it != cache_.end())
        return it->second;

    GlyphBmp g;
    stbtt_fontinfo info{};
    const auto& data = ttfData(bold);
    if (data.empty())  // fonts missing: draw nothing instead of crashing
        return g;
    if (!stbtt_InitFont(&info, data.data(), stbtt_GetFontOffsetForIndex(data.data(), 0)))
        return g;
    const float scale = stbtt_ScaleForPixelHeight(&info, size);
    int use = cp;
    if (stbtt_FindGlyphIndex(&info, cp) == 0) {
        const int fb = asciiFallback(cp);
        if (fb > 0 && stbtt_FindGlyphIndex(&info, fb) != 0)
            use = fb;
        else if (cp != '?' && stbtt_FindGlyphIndex(&info, '?') != 0)
            use = '?';
    }
    int advance = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&info, use, &advance, &lsb);
    g.advance = advance * scale;

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBitmapBox(&info, use, scale, scale, &x0, &y0, &x1, &y1);
    g.w = x1 - x0;
    g.h = y1 - y0;
    g.xoff = x0;
    g.yoff = y0;
    if (g.w > 0 && g.h > 0) {
        g.gray.resize(static_cast<std::size_t>(g.w * g.h));
        stbtt_MakeCodepointBitmap(&info, g.gray.data(), g.w, g.h, g.w, scale, scale, use);
    }
    cache_[key] = g;
    return g;
}

void FontSet::drawText(std::uint8_t* rgba, int stride, int w, int h, float x, float y,
                       const std::string& text, float pixelHeight, bool bold, std::uint8_t cr,
                       std::uint8_t cg, std::uint8_t cb, std::uint8_t ca) const {
    float penX = x;
    const float baseline = y + pixelHeight * 0.8f;
    for (size_t i = 0; i < text.size();) {
        const int cp = utf8Codepoint(text, i);
        if (cp < 0)
            break;
        if (cp == '\n')
            continue;
        const GlyphBmp g = rasterGlyph(cp, pixelHeight, bold);
        const int gx = static_cast<int>(std::round(penX + g.xoff));
        const int gy = static_cast<int>(std::round(baseline + g.yoff));
        for (int row = 0; row < g.h; ++row) {
            const int py = gy + row;
            if (py < 0 || py >= h)
                continue;
            for (int col = 0; col < g.w; ++col) {
                const int px = gx + col;
                if (px < 0 || px >= w)
                    continue;
                const std::uint8_t a = static_cast<std::uint8_t>(
                    (static_cast<int>(g.gray[static_cast<std::size_t>(row * g.w + col)]) * ca) / 255);
                if (a == 0)
                    continue;
                std::uint8_t* dst = rgba + (static_cast<std::size_t>(py) * stride + px * 4);
                const int inv = 255 - a;
                dst[0] = static_cast<std::uint8_t>((cr * a + dst[0] * inv) / 255);
                dst[1] = static_cast<std::uint8_t>((cg * a + dst[1] * inv) / 255);
                dst[2] = static_cast<std::uint8_t>((cb * a + dst[2] * inv) / 255);
                dst[3] = static_cast<std::uint8_t>(std::min(255, dst[3] + a));
            }
        }
        penX += g.advance;
    }
}

float FontSet::measureText(const std::string& text, float pixelHeight, bool bold) const {
    float w = 0.f;
    for (size_t i = 0; i < text.size();) {
        const int cp = utf8Codepoint(text, i);
        if (cp < 0)
            break;
        if (cp == '\n')
            continue;
        w += rasterGlyph(cp, pixelHeight, bold).advance;
    }
    return w;
}

namespace {

constexpr const char* kEllipsis = "\xe2\x80\xa6"; // U+2026

std::string trimEllipsisLine(const FontSet& fonts, std::string line, float maxWidth,
                             float pixelHeight, bool bold) {
    if (line.empty())
        return kEllipsis;
    const std::string suffix = std::string(" ") + kEllipsis;
    while (!line.empty()) {
        if (fonts.measureText(line + suffix, pixelHeight, bold) <= maxWidth)
            return line + suffix;
        const size_t sp = line.find_last_of(' ');
        if (sp != std::string::npos && sp > 0)
            line.resize(sp);
        else
            line.clear();
    }
    return kEllipsis;
}

} // namespace

std::string FontSet::ellipsize(const std::string& text, float pixelHeight, bool bold,
                               float maxWidth) const {
    const std::string ellipsis = "\xE2\x80\xA6";
    if (text.empty() || maxWidth <= 0.f)
        return text;
    // One pass over codepoints. Prefix widths are cumulative, so the cut is
    // linear instead of re-measuring the whole string per dropped codepoint.
    struct Mark {
        std::size_t end = 0;
        float width = 0.f;
    };
    std::vector<Mark> marks;
    marks.reserve(text.size());
    float total = 0.f;
    for (std::size_t i = 0; i < text.size();) {
        const int cp = utf8Codepoint(text, i);
        if (cp < 0)
            break;
        if (cp != '\n')
            total += rasterGlyph(cp, pixelHeight, bold).advance;
        marks.push_back(Mark{i, total});
    }
    if (total <= maxWidth)
        return text;
    const float ellW = measureText(ellipsis, pixelHeight, bold);
    std::size_t keep = 0;
    for (const Mark& m : marks) {
        if (m.width + ellW <= maxWidth)
            keep = m.end;
        else
            break;
    }
    if (keep == 0)
        return ellipsis;
    return text.substr(0, keep) + ellipsis;
}

float FontSet::measureTextWrapped(const std::string& text, float pixelHeight, bool bold,
                                  float maxWidth, int maxLines) const {
    if (maxLines <= 0 || text.empty())
        return 0.f;
    const float lineStep = pixelHeight * 1.35f;
    std::string word;
    std::string line;
    int lines = 0;
    bool truncated = false;
    for (size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : ' ';
        if (c != ' ' && c != '\n' && i < text.size()) {
            word += c;
            continue;
        }
        if (!word.empty()) {
            const std::string trial = line.empty() ? word : line + " " + word;
            if (measureText(trial, pixelHeight, bold) > maxWidth && !line.empty()) {
                ++lines;
                if (lines >= maxLines) {
                    truncated = true;
                    break;
                }
                line = word;
            } else {
                line = trial;
            }
            word.clear();
        }
        if (c == '\n') {
            ++lines;
            line.clear();
            if (lines >= maxLines) {
                truncated = true;
                break;
            }
        }
    }
    if (!truncated) {
        if (!line.empty())
            ++lines;
        lines = std::min(lines, maxLines);
        return static_cast<float>(lines) * lineStep;
    }
    return static_cast<float>(maxLines) * lineStep;
}

void FontSet::drawTextWrapped(std::uint8_t* rgba, int stride, int w, int h, float x, float y,
                              float maxWidth, const std::string& text, float pixelHeight, bool bold,
                              std::uint8_t cr, std::uint8_t cg, std::uint8_t cb, int maxLines,
                              std::uint8_t ca) const {
    if (maxLines <= 0 || text.empty())
        return;
    std::string word;
    std::string line;
    int lines = 0;
    float lineY = y;
    const float lineStep = pixelHeight * 1.35f;
    auto emitLine = [&](std::string content, bool ellipsis) {
        if (content.empty())
            return;
        if (ellipsis)
            content = trimEllipsisLine(*this, content, maxWidth, pixelHeight, bold);
        drawText(rgba, stride, w, h, x, lineY, content, pixelHeight, bold, cr, cg, cb, ca);
        lineY += lineStep;
        ++lines;
    };
    for (size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : ' ';
        const bool atEnd = i >= text.size();
        if (c != ' ' && c != '\n' && !atEnd) {
            word += c;
            continue;
        }
        if (!word.empty()) {
            const std::string trial = line.empty() ? word : line + " " + word;
            if (measureText(trial, pixelHeight, bold) > maxWidth && !line.empty()) {
                if (lines + 1 >= maxLines) {
                    emitLine(line, true);
                    return;
                }
                emitLine(line, false);
                line = word;
            } else {
                line = trial;
            }
            word.clear();
        }
        if (c == '\n' || atEnd) {
            if (!line.empty()) {
                const bool moreComing = !atEnd && c == '\n';
                if (lines + 1 >= maxLines) {
                    emitLine(line, moreComing || !atEnd);
                    return;
                }
                emitLine(line, false);
                line.clear();
            }
            if (atEnd)
                return;
        }
    }
}

} // namespace plexui
} // namespace misterplex
