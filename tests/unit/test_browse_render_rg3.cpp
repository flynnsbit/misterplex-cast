#include "browse_render_test_common.hpp"

#include <iostream>

int main() {
    try {
        int screens = 0;
        int outside = 0;
        for (const auto& item : brtest::goldenModels()) {
            const auto rgb = brtest::render(item.second);
            for (int y = 0; y < brtest::H; ++y) {
                for (int x = 0; x < brtest::W; ++x) {
                    if (brtest::isBackgroundAt(rgb, x, y))
                        continue;
                    if (!brtest::allowedOutsideTitleSafe(x, y, rgb)) {
                        ++outside;
                        if (outside < 10)
                            std::cerr << item.first << " unsafe pixel at " << x << "," << y << "\n";
                    }
                }
            }
            ++screens;
        }
        brtest::require(outside == 0, "safe-area violations");
        std::cout << "RG-3 PASS screens=" << screens << " unsafe_pixels=0\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "RG-3 FAIL: " << e.what() << "\n";
        return 1;
    }
}
