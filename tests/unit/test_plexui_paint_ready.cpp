// Reproduces the PlexUI black-first-paint class: browse returns grid data (HTTP 200) while
// poster/backdrop URLs are still decoding; paintIdle must defer rather than present idle black.
#include "libmisterplex/browse_controller.hpp"
#include "libmisterplex/browse_fetch.hpp"
#include "libmisterplex/plexui/plexui_controller.hpp"
#include "libmisterplex/plexui/plexui_data.hpp"
#include "libmisterplex/plexui/plexui_idle_paint.hpp"
#include "libmisterplex/plexui/plexui_images.hpp"
#include "libmisterplex/plexui/plexui_render.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace misterplex::plexui;

namespace {
int g_checks = 0;
int g_fail = 0;
#define CHECK(cond, ...)                                                                    \
    do {                                                                                    \
        ++g_checks;                                                                         \
        if (!(cond)) {                                                                      \
            ++g_fail;                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);          \
            std::fprintf(stderr, __VA_ARGS__);                                              \
            std::fprintf(stderr, "\n");                                                     \
        }                                                                                   \
    } while (0)

PlexServer labServer() {
    PlexServer server;
    server.name = "Lab";
    server.accessToken = "tok";
    server.baseUrl = "http://127.0.0.1:32400";
    server.machineIdentifier = "lab-mid";
    return server;
}

void initGridController(PlexUiController& ui, PlexServer server) {
    std::vector<PlexItem> grid;
    for (int i = 0; i < 12; ++i) {
        PlexItem it;
        it.title = "Movie " + std::to_string(i);
        it.thumb = "/library/metadata/" + std::to_string(100 + i) + "/thumb/1";
        it.art = it.thumb;
        it.ratingKey = std::to_string(100 + i);   // rc3: the keyed cache needs a ratingKey
        grid.push_back(std::move(it));
    }
    ui.testSetLibraryGrid(std::move(server), std::move(grid), {});
}

std::unique_ptr<PlexUiController> newGridController(PlexServer server) {
    PlexUiController::Config cfg;
    cfg.fontDir = "assets/plexui-fonts";
    PlexUiController::Hooks hooks;
    hooks.get = [](const std::string&, int, int, const misterplex::BrowseCancelFn&) {
        return misterplex::BrowseHttpResult{};
    };
    auto ui = std::make_unique<PlexUiController>(std::move(cfg), std::move(hooks));
    initGridController(*ui, std::move(server));
    return ui;
}

void seedVisibleGridArt(PlexUiController& ui, const PlexServer& server) {
    const GridLayout gridLayout = computeGridLayout(true);
    const int pw = gridLayout.posterW;
    const int ph = gridLayout.posterH;
    const int visibleRows = gridVisibleRowCount(gridLayout);
    const int n = visibleRows * gridLayout.cols;
    for (int i = 0; i < n; ++i) {
        const std::string rk = std::to_string(100 + i);
        PlexItem it;
        it.ratingKey = rk;
        it.thumb = "/library/metadata/" + rk + "/thumb/1";
        it.art = it.thumb;
        ui.testInsertImage(thumbCacheKey(server.machineIdentifier, rk, pw, ph, itemThumbRevision(it)));
    }
    PlexItem focus;
    focus.ratingKey = "100";
    focus.thumb = "/library/metadata/100/thumb/1";
    focus.art = focus.thumb;
    ui.testInsertImage(
        artCacheKey(server.machineIdentifier, "100", 160, 90, itemArtRevision(focus)));}

void test_grid_paint_ready_gates_art() {
    PlexServer server = labServer();
    auto ui = newGridController(std::move(server));
    server = labServer();
    CHECK(ui->visible(), "grid injected visible");
    CHECK(ui->frameSummary().view == "grid", "view grid");
    CHECK(ui->frameSummary().grid == 12, "grid count");
    CHECK(!ui->paintReady(), "not ready before art decode");

    seedVisibleGridArt(*ui, server);
    CHECK(ui->paintReady(), "ready after visible art seeded");
}

void test_idle_defer_while_plex_not_ready() {
    PlexServer server = labServer();
    auto ui = newGridController(std::move(server));
    server = labServer();
    CHECK(deferIdlePaintOverPlexUi(true, true, ui->paintReady()),
          "defer idle paint while grid art pending");
    seedVisibleGridArt(*ui, server);
    CHECK(!deferIdlePaintOverPlexUi(true, true, ui->paintReady()),
          "paint idle once grid art ready");
    CHECK(!deferIdlePaintOverPlexUi(false, true, false), "no plex -> no defer");
}

} // namespace

int main() {
    test_grid_paint_ready_gates_art();
    test_idle_defer_while_plex_not_ready();
    if (g_fail) {
        std::fprintf(stderr, "test_plexui_paint_ready: %d/%d checks failed\n", g_fail, g_checks);
        return 1;
    }
    std::fprintf(stderr, "test_plexui_paint_ready: %d checks ok\n", g_checks);
    return 0;
}
