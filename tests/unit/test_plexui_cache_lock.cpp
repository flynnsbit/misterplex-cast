// copyFrame and paintReady hold mu_ and call scheduleImages -> updateImageCacheBudget.
// A nested lock_guard(mu_) deadlocks (glibc PTHREAD_MUTEX_NORMAL). alarm() fails the
// process if that happens, so CI does not stall. MUT_PLEXUI_CACHE_RELOCK puts the lock back.
#include "libmisterplex/plexui/plexui_controller.hpp"

#include <csignal>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

using namespace misterplex::plexui;

namespace {
int g_checks = 0;
int g_fail = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            ++g_fail;                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                           \
    } while (0)

void arm(unsigned sec) {
    std::signal(SIGALRM, SIG_DFL);
    ::alarm(sec);
}
} // namespace

int main() {
    PlexUiController::Config cfg;
    cfg.fontDir = "assets/plexui-fonts";
    PlexUiController::Hooks hooks;
    hooks.get = [](const std::string&, int, int, const misterplex::BrowseCancelFn&) {
        return misterplex::BrowseHttpResult{};
    };
    PlexUiController ui(std::move(cfg), std::move(hooks));
    PlexServer server;
    server.name = "Lab";
    server.machineIdentifier = "lab-machine";
    server.accessToken = "tok";
    server.baseUrl = "http://127.0.0.1:32400";
    std::vector<PlexItem> grid;
    for (int i = 0; i < 4; ++i) {
        PlexItem it;
        it.ratingKey = std::to_string(100 + i);
        it.title = "Movie " + std::to_string(i);
        it.thumb = "/library/metadata/" + it.ratingKey + "/thumb/1";
        grid.push_back(std::move(it));
    }
    ui.testSetLibraryGrid(std::move(server), std::move(grid), {});

    arm(3);
    std::vector<std::uint8_t> frame;
    const bool copied = ui.copyFrame(frame);
    const bool ready = ui.paintReady();
    ::alarm(0);
    CHECK(copied);
    CHECK(!frame.empty());
    (void)ready;
    std::printf("test_plexui_cache_lock: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
