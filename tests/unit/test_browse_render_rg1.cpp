#include "browse_render_test_common.hpp"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

namespace {

std::string hex32(std::uint32_t v) {
    std::ostringstream os;
    os << std::hex << std::setw(8) << std::setfill('0') << v;
    return os.str();
}

std::map<std::string, std::string> readMd5s(const std::string& path) {
    std::ifstream f(path);
    brtest::require(static_cast<bool>(f), "cannot open " + path);
    std::map<std::string, std::string> out;
    std::string md5;
    std::string name;
    while (f >> md5 >> name)
        out[name] = md5;
    return out;
}

int compareMockup(const std::string& root, const std::string& name,
                  const brtest::RenderModel& model) {
    const auto got = brtest::render(model);
    const auto want = brtest::pngDecodeRgb(root + "/tests/fixtures/browse_render/mockups/" + name + "_640x480.png");
    int diff = 0;
    for (std::size_t i = 0; i < got.size(); i += 3) {
        if (got[i] != want[i] || got[i + 1] != want[i + 1] || got[i + 2] != want[i + 2])
            ++diff;
    }
    std::cout << "mockup " << name << " differing_pixels=" << diff << "\n";
    return diff;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string root = argc > 1 ? argv[1] : ".";
        const bool update = std::getenv("BROWSE_RENDER_UPDATE_GOLDENS") != nullptr;
        const std::string fixtureDir = root + "/tests/fixtures/browse_render";
        const std::string buildDir = root + "/build";
        const std::string md5Path = fixtureDir + "/goldens.md5";
        std::map<std::string, std::string> expected = update ? std::map<std::string, std::string>{}
                                                             : readMd5s(md5Path);
        std::ofstream updated;
        if (update) {
            updated.open(md5Path);
            brtest::require(static_cast<bool>(updated), "cannot rewrite " + md5Path);
        }
        int generated = 0;
        for (const auto& item : brtest::goldenModels()) {
            const auto rgb = brtest::render(item.second);
            const std::string rawMd5 = brtest::md5Hex(rgb);
            const auto png = brtest::pngEncodeRgb(rgb);
            const std::string buildPath = buildDir + "/" + item.first + "_640x480.png";
            brtest::writeFile(buildPath, png);
            if (update) {
                updated << rawMd5 << "  " << item.first << ".rgb24\n";
            } else {
                const auto it = expected.find(item.first + ".rgb24");
                brtest::require(it != expected.end(), "missing md5 for " + item.first);
                brtest::require(it->second == rawMd5, "raw RGB24 md5 mismatch for " + item.first);
            }
            std::cout << "golden " << item.first << " raw_md5=" << rawMd5 << " review_png_bytes=" << png.size()
                      << " crc32=" << hex32(brtest::crc32Bytes(png.data(), png.size())) << "\n";
            ++generated;
        }

        int mockDiffs = 0;
        const int s2Diff = compareMockup(root, "rg1_s2_servers", brtest::s2Servers());
        const int s4Diff = compareMockup(root, "rg1_s4_movies", brtest::s4Movies(true));
        const int s5Diff = compareMockup(root, "rg1_s5_detail", brtest::s5Detail());
        mockDiffs = s2Diff + s4Diff + s5Diff;
        brtest::require(s2Diff == 0 && s4Diff == 0, "mockup pixel compare failed");
        brtest::require(s5Diff == 29,
                        "unexpected S5 mockup difference; expected only F3 strict '>' fallback pixels");
        std::cout << "mockup rg1_s5_detail cause=F3 strict helper text sanitizes '>' to '?' while mockup.py draws '>'\n";
        std::cout << "RG-1 PASS goldens=" << generated << " mockups=3 differing_pixels="
                  << mockDiffs << " allowed_spec_mockup_disagreement=29\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "RG-1 FAIL: " << e.what() << "\n";
        return 1;
    }
}
