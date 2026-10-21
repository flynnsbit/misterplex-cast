// Host screenshot tool for Plex UI iteration (live PMS + token file).
#include "libmisterplex/plexui/plexui_config.hpp"
#include "libmisterplex/plexui/plexui_controller.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

misterplex::BrowseOp mapKey(char c) {
    switch (c) {
    case 'u':
    case 'U':
        return misterplex::BrowseOp::Up;
    case 'd':
    case 'D':
        return misterplex::BrowseOp::Down;
    case 'l':
    case 'L':
    case 'r':
    case 'R':
        return misterplex::BrowseOp::Open;
    case 's':
    case 'S':
    case '\n':
        return misterplex::BrowseOp::Select;
    case 'b':
    case 'B':
    case 27:
        return misterplex::BrowseOp::Back;
    case 'o':
    case 'O':
        return misterplex::BrowseOp::Open;
    default:
        return misterplex::BrowseOp::Open;
    }
}

void printSummary(int shot, const misterplex::plexui::PlexUiController::FrameSummary& s) {
    std::fprintf(stderr,
                 "plexui_shot: frame %d view=%s servers=%d sections=%d hubs=%d hub_items=%d "
                 "grid=%d loading=%s image_bytes=%zu\n",
                 shot, s.view.c_str(), s.servers, s.sections, s.hubs, s.hubItems, s.grid,
                 s.loading ? "yes" : "no", s.imageBytes);
}

bool writeFrame(misterplex::plexui::PlexUiController& ui, const std::string& path, int& shot) {
    constexpr int kWaitMs = 45000;
    if (!ui.waitForFrame(kWaitMs)) {
        std::fprintf(stderr, "plexui_shot: frame %d wait timeout (%d ms)\n", shot, kWaitMs);
    }
    const auto summary = ui.frameSummary();
    printSummary(shot, summary);
    std::vector<std::uint8_t> rgb;
    if (!ui.copyFrame(rgb) || rgb.empty()) {
        std::fprintf(stderr, "plexui_shot: frame %d capture skipped (not visible)\n", shot);
        return false;
    }
    if (stbi_write_png(path.c_str(), misterplex::plexui::kScreenW, misterplex::plexui::kScreenH, 3,
                       rgb.data(), misterplex::plexui::kScreenW * 3)) {
        std::fprintf(stderr, "plexui_shot: wrote %s\n", path.c_str());
        ++shot;
        return true;
    }
    return false;
}

struct Options {
    std::string tokenFile;
    std::string keys;
    std::string outPrefix;
    std::string fontDir;
    std::string serverNeedle;
    std::string sectionNeedle = "Movies";
    std::string libraryNeedle;
    std::string suiteDir;
    bool categoriesShot = false;
    bool sidebarShot = false;
};

bool parseArgs(int argc, char** argv, Options& opt) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <token-file> [<key-sequence> <out-prefix>] [font-dir]\n"
                     "       %s --server=NAME --section=LIB --suite=DIR <token-file>\n"
                     "       %s --sidebar --library=Title/subtitle --suite=DIR <token-file>\n"
                     " keys: u/d/l/r s(select) b(back) o(open) .(pause 400ms)\n"
                     " opts: --server=substr  --section=substr  --library=Title/owner\n"
                     "       --suite=outdir  --sidebar  --categories\n",
                     argv[0], argv[0], argv[0]);
        return false;
    }
    int pos = 1;
    for (; pos < argc; ++pos) {
        const char* a = argv[pos];
        if (std::strncmp(a, "--server=", 9) == 0) {
            opt.serverNeedle = a + 9;
            continue;
        }
        if (std::strncmp(a, "--section=", 10) == 0) {
            opt.sectionNeedle = a + 10;
            continue;
        }
        if (std::strncmp(a, "--library=", 10) == 0) {
            opt.libraryNeedle = a + 10;
            continue;
        }
        if (std::strncmp(a, "--suite=", 8) == 0) {
            opt.suiteDir = a + 8;
            continue;
        }
        if (std::strcmp(a, "--categories") == 0) {
            opt.categoriesShot = true;
            continue;
        }
        if (std::strcmp(a, "--sidebar") == 0) {
            opt.sidebarShot = true;
            continue;
        }
        break;
    }
    if (pos >= argc)
        return false;
    opt.tokenFile = argv[pos++];
    if (!opt.suiteDir.empty()) {
        if (pos < argc)
            opt.fontDir = argv[pos];
        return true;
    }
    if (pos + 1 >= argc)
        return false;
    opt.keys = argv[pos++];
    opt.outPrefix = argv[pos++];
    if (pos < argc)
        opt.fontDir = argv[pos];
    return true;
}

bool runSuite(misterplex::plexui::PlexUiController& ui, const Options& opt, int& shot) {
    ui.post(misterplex::BrowseOp::Open);
    if (opt.sidebarShot) {
        constexpr int kWaitMs = 45000;
        if (!ui.waitForFrame(kWaitMs))
            std::fprintf(stderr, "plexui_shot: sidebar wait timeout (%d ms)\n", kWaitMs);
        if (!ui.shotFocusSidebarIndex(1))
            std::fprintf(stderr, "plexui_shot: sidebar focus index 1 unavailable\n");
        return writeFrame(ui, opt.suiteDir + "/sidebar.png", shot);
    }

    if (!opt.libraryNeedle.empty()) {
        std::string tag = opt.libraryNeedle;
        for (char& c : tag) {
            if (c == '/')
                c = '_';
        }
        if (!writeFrame(ui, opt.suiteDir + "/home.png", shot))
            return false;
        if (!ui.shotFocusLibrary(opt.libraryNeedle)) {
            std::fprintf(stderr, "plexui_shot: library '%s' not found\n", opt.libraryNeedle.c_str());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ui.post(misterplex::BrowseOp::Select);
        constexpr int kGridWaitMs = 300000;
        if (!ui.waitForFrame(kGridWaitMs) || ui.frameSummary().view != "grid") {
            std::fprintf(stderr, "plexui_shot: grid view timeout for '%s'\n",
                         opt.libraryNeedle.c_str());
            return false;
        }
        if (!writeFrame(ui, opt.suiteDir + "/grid_" + tag + ".png", shot))
            return false;
        ui.post(misterplex::BrowseOp::Select);
        constexpr int kDetailWaitMs = 90000;
        if (!ui.waitForFrame(kDetailWaitMs) || ui.frameSummary().view != "detail") {
            std::fprintf(stderr, "plexui_shot: detail view timeout for '%s'\n",
                         opt.libraryNeedle.c_str());
            return false;
        }
        return writeFrame(ui, opt.suiteDir + "/detail_" + tag + ".png", shot);
    }

    ui.shotFocusServer(opt.serverNeedle);
    ui.post(misterplex::BrowseOp::Select);
    if (!writeFrame(ui, opt.suiteDir + "/servers.png", shot))
        return false;

    if (!writeFrame(ui, opt.suiteDir + "/home.png", shot))
        return false;

    if (!ui.shotFocusSection(opt.sectionNeedle)) {
        std::fprintf(stderr, "plexui_shot: section '%s' not found\n", opt.sectionNeedle.c_str());
        return false;
    }
    ui.post(misterplex::BrowseOp::Select);
    if (!writeFrame(ui, opt.suiteDir + "/grid.png", shot))
        return false;

    if (opt.categoriesShot) {
        ui.post(misterplex::BrowseOp::Up);
        ui.postHorizontal(1);
        ui.postHorizontal(1);
        ui.post(misterplex::BrowseOp::Select);
        if (!writeFrame(ui, opt.suiteDir + "/categories.png", shot))
            return false;
        ui.post(misterplex::BrowseOp::Down);
    }

    ui.post(misterplex::BrowseOp::Select);
    return writeFrame(ui, opt.suiteDir + "/detail.png", shot);
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt))
        return 2;

    const std::string token = misterplex::plexui::readTokenFile(opt.tokenFile);
    if (token.empty()) {
        std::fprintf(stderr, "plexui_shot: empty token file %s\n", opt.tokenFile.c_str());
        return 1;
    }
    std::string fontDir =
        opt.fontDir.empty() ? misterplex::plexui::plexUiDirFromEnvironment() : opt.fontDir;

    misterplex::plexui::PlexUiController::Config cfg;
    cfg.accountToken = token;
    cfg.fontDir = fontDir;
    misterplex::plexui::PlexUiController::Hooks hooks;
    hooks.log = [](const std::string& line) { std::fprintf(stderr, "%s\n", line.c_str()); };
    misterplex::plexui::PlexUiController ui(cfg, std::move(hooks));
    ui.setShotPreferences(opt.serverNeedle, opt.sectionNeedle);
    ui.start();

    int shot = 0;
    if (!opt.suiteDir.empty()) {
        const int rc = runSuite(ui, opt, shot) ? 0 : 1;
        ui.stop();
        return rc;
    }

    auto capture = [&] {
        const std::string path = opt.outPrefix + std::to_string(shot) + ".png";
        if (writeFrame(ui, path, shot))
            ++shot;
    };

    for (const char* p = opt.keys.c_str(); *p; ++p) {
        if (*p == '.') {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            continue;
        }
        if (*p == 'h' || *p == 'H')
            ui.postHorizontal(1);
        else if (*p == 'j' || *p == 'J')
            ui.postHorizontal(-1);
        else if (*p == 'l' || *p == 'L')
            ui.postHorizontal(-1);
        else if (*p == 'r' || *p == 'R')
            ui.postHorizontal(1);
        else
            ui.post(mapKey(*p));
        capture();
    }

    ui.stop();
    return 0;
}
