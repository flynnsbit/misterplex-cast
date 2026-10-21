#include "browse_render_test_common.hpp"

#include <iostream>
#include <random>

int main() {
    try {
        const std::string printable =
            " !#$%'()*+,-./0123456789:;=?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
        brtest::require(misterplex::browse::sanitizeText(printable) == printable,
                        "safe printable fixture string changed");
        brtest::require(misterplex::browse::sanitizeText("<>&\"") == "????",
                        "unsafe XML attribute chars did not become fallback glyphs");
        std::string allBytes;
        for (int i = 0; i < 256; ++i)
            allBytes.push_back(static_cast<char>(i));
        const auto sanitized = misterplex::browse::sanitizeText(allBytes);
        int fallbacks = 0;
        for (char c : sanitized) {
            if (c == '?')
                ++fallbacks;
        }
        brtest::require(fallbacks >= 165, "byte fuzz did not produce expected fallback glyphs");

        std::mt19937 rng(0xBEEF1004u);
        for (int i = 0; i < 200; ++i) {
            brtest::RenderModel m = brtest::s5Detail();
            m.posterPanel = true;
            std::vector<std::uint8_t> badPoster(static_cast<std::size_t>(rng() % 4096u), 0xA5);
            m.posterBlob = badPoster.data();
            m.posterBlobSize = badPoster.size();
            m.headerTitle = allBytes + std::string(300, 'X');
            m.footerText = allBytes + std::string(300, 'Y');
            m.rows.clear();
            for (int r = 0; r < 24; ++r) {
                std::string s;
                const int len = static_cast<int>(rng() % 220u);
                for (int j = 0; j < len; ++j)
                    s.push_back(static_cast<char>(rng() & 0xFFu));
                m.rows.push_back({r % 5 == 0 ? brtest::RowKind::Info : brtest::RowKind::Directory, s,
                                  static_cast<int>(rng() % 130u) - 10, (rng() & 1u) != 0});
            }
            m.cursorIndex = static_cast<int>(rng() % m.rows.size());
            m.viewportTop = static_cast<int>(rng() % m.rows.size());
            const auto rgb = brtest::render(m);
            for (int y = 0; y < brtest::H; ++y) {
                for (int x = 0; x < brtest::W; ++x) {
                    if (brtest::isBackgroundAt(rgb, x, y))
                        continue;
                    brtest::require(brtest::allowedOutsideTitleSafe(x, y, rgb),
                                    "fuzz drew outside row/column budget");
                }
            }
        }
        std::cout << "RG-4 PASS printable=1 fuzz_cases=200 invalid_fallbacks=" << fallbacks << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "RG-4 FAIL: " << e.what() << "\n";
        return 1;
    }
}
