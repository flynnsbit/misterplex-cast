#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace misterplex {
namespace plexui {

class FontSet {
public:
    bool load(const std::string& dir, std::string& err);
    bool ready() const { return usingFallback_ || (!regularData_.empty() && !boldData_.empty()); }
    bool usingFallback() const { return usingFallback_; }

    void drawText(std::uint8_t* rgba, int stride, int w, int h, float x, float y,
                  std::string_view text, float pixelHeight, bool bold, std::uint8_t r,
                  std::uint8_t g, std::uint8_t b, std::uint8_t a = 255,
                  int clipX0 = 0, int clipY0 = 0,
                  int clipX1 = std::numeric_limits<int>::max(),
                  int clipY1 = std::numeric_limits<int>::max()) const;

    void drawTextWrapped(std::uint8_t* rgba, int stride, int w, int h, float x, float y,
                         float maxWidth, const std::string& text, float pixelHeight, bool bold,
                         std::uint8_t r, std::uint8_t g, std::uint8_t b, int maxLines,
                         std::uint8_t a = 255) const;

    float measureText(std::string_view text, float pixelHeight, bool bold) const;
    std::size_t fittingPrefix(std::string_view text, float pixelHeight, bool bold, float maxWidth) const;

    // Tight ink metrics of one codepoint, same raster as drawText (xoff/yoff relative
    // to the pen and to the baseline = y + pixelHeight * 0.8).
    struct GlyphBox {
        int w = 0, h = 0;
        int xoff = 0, yoff = 0;
        float advance = 0.f;
    };
    GlyphBox measureGlyph(int codepoint, float pixelHeight, bool bold) const;

    // Tight ink box of `text` as drawText would paint it at (x, y).
    // x0/y0 inclusive, x1/y1 exclusive. Returns false if there is no ink.
    bool measureTextBox(const std::string& text, float pixelHeight, bool bold, float x, float y,
                        int& x0, int& y0, int& x1, int& y1) const;

    // Drop whole codepoints from the end until `text` plus an ellipsis fits.
    std::string ellipsize(const std::string& text, float pixelHeight, bool bold,
                          float maxWidth) const;

    // Returns total height from y (line height * lines used); respects maxLines and word-safe ellipsis.
    float measureTextWrapped(const std::string& text, float pixelHeight, bool bold, float maxWidth,
                             int maxLines) const;

private:
    struct GlyphKey {
        int codepoint = 0;
        float size = 0.f;
        bool bold = false;
        bool operator==(const GlyphKey& o) const {
            return codepoint == o.codepoint && size == o.size && bold == o.bold;
        }
    };
    struct GlyphKeyHash {
        std::size_t operator()(const GlyphKey& k) const {
            return static_cast<std::size_t>(k.codepoint) ^ (static_cast<std::size_t>(k.size * 10) << 1) ^
                   (k.bold ? 3u : 0u);
        }
    };
    struct GlyphBmp {
        int w = 0, h = 0;
        int xoff = 0, yoff = 0;
        float advance = 0.f;
        std::vector<std::uint8_t> gray;
    };

    const std::vector<std::uint8_t>& ttfData(bool bold) const;
    const GlyphBmp& rasterGlyph(int cp, float size, bool bold) const;
    GlyphBmp rasterFallbackGlyph(int cp, float size, bool bold) const;

    std::vector<std::uint8_t> regularData_;
    std::vector<std::uint8_t> boldData_;
    bool usingFallback_ = false;
    mutable std::unordered_map<GlyphKey, GlyphBmp, GlyphKeyHash> cache_;
};

} // namespace plexui
} // namespace misterplex
