#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace misterplex {
namespace plexui {

class FontSet {
public:
    bool load(const std::string& dir, std::string& err);
    bool ready() const { return !regularData_.empty() && !boldData_.empty(); }

    void drawText(std::uint8_t* rgba, int stride, int w, int h, float x, float y,
                  const std::string& text, float pixelHeight, bool bold, std::uint8_t r,
                  std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) const;

    void drawTextWrapped(std::uint8_t* rgba, int stride, int w, int h, float x, float y,
                         float maxWidth, const std::string& text, float pixelHeight, bool bold,
                         std::uint8_t r, std::uint8_t g, std::uint8_t b, int maxLines,
                         std::uint8_t a = 255) const;

    float measureText(const std::string& text, float pixelHeight, bool bold) const;

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
    GlyphBmp rasterGlyph(int cp, float size, bool bold) const;

    std::vector<std::uint8_t> regularData_;
    std::vector<std::uint8_t> boldData_;
    mutable std::unordered_map<GlyphKey, GlyphBmp, GlyphKeyHash> cache_;
};

} // namespace plexui
} // namespace misterplex
