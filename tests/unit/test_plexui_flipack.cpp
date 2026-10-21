// onR1FlipAck is called from paintIdleR1 while r1PaintMu_ is held, and from paintIdleV2
// on the same thread that will re-enter copyFrame. hooks_.repaint is paintIdle.
// A synchronous repaint from inside onR1FlipAck deadlocks. This test holds a stand-in
// paint mutex across onR1FlipAck; the repaint hook takes that mutex and then copyFrame.
// MUT_PLEXUI_FLIPACK_REPAINT restores the call and the alarm kills the process.
// The worker must still reach focus frame 3 after two acks, without waiting for the
// 30 s idle repaint.
#include "libmisterplex/plexui/plexui_controller.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <mutex>
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

PlexUiController::Hooks makeHooks(std::mutex& paintMu, std::atomic<int>& repaints, PlexUiController*& raw) {
    PlexUiController::Hooks hooks;
    hooks.get = [](const std::string&, int, int, const misterplex::BrowseCancelFn&) {
        return misterplex::BrowseHttpResult{};
    };
    hooks.repaint = [&] {
        std::lock_guard<std::mutex> lk(paintMu);
        repaints.fetch_add(1);
        if (!raw)
            return;
        std::vector<std::uint8_t> frame;
        raw->copyFrame(frame);
    };
    return hooks;
}

void seedGrid(PlexUiController& ui) {
    PlexServer server;
    server.name = "Lab";
    server.machineIdentifier = "lab-machine";
    server.accessToken = "tok";
    server.baseUrl = "http://127.0.0.1:32400";
    std::vector<PlexItem> grid(1);
    grid[0].ratingKey = "100";
    grid[0].title = "Movie";
    grid[0].thumb = "/library/metadata/100/thumb/1";
    ui.testSetLibraryGrid(std::move(server), std::move(grid), {});
}

bool waitRepaints(std::atomic<int>& repaints, int n) {
    for (int i = 0; i < 200 && repaints.load() < n; ++i)
        ::usleep(10000);
    return repaints.load() >= n;
}
} // namespace

int main() {
    std::signal(SIGALRM, SIG_DFL);

    // No synchronous repaint while the paint mutex is held. The worker is not
    // started, so a notify cannot paint on another thread before the check.
    {
        std::mutex paintMu;
        std::atomic<int> repaints{0};
        PlexUiController* raw = nullptr;
        PlexUiController::Config cfg;
        cfg.fontDir = "assets/plexui-fonts";
        PlexUiController ui(cfg, makeHooks(paintMu, repaints, raw));
        raw = &ui;
        seedGrid(ui);
        ui.testArmAnimAck();
        ::alarm(3);
        {
            std::lock_guard<std::mutex> held(paintMu);
            ui.onR1FlipAck(10, 20);
            CHECK(repaints.load() == 0);
            CHECK(ui.testFocusAnim() == 1);
        }
        ::alarm(0);
        CHECK(repaints.load() == 0);
    }

    // Two acks with the worker running: frame 1 -> 2 -> 3, one repaint each.
    {
        std::mutex paintMu;
        std::atomic<int> repaints{0};
        PlexUiController* raw = nullptr;
        PlexUiController::Config cfg;
        cfg.fontDir = "assets/plexui-fonts";
        PlexUiController ui(cfg, makeHooks(paintMu, repaints, raw));
        raw = &ui;
        seedGrid(ui);
        ui.testArmAnimAt(1);
        ui.start();
        ::alarm(3);
        ui.onR1FlipAck(10, 20);
        CHECK(waitRepaints(repaints, 1));
        ui.onR1FlipAck(11, 21);
        CHECK(waitRepaints(repaints, 2));
        ::alarm(0);
        CHECK(repaints.load() == 2);
        CHECK(ui.testFocusAnim() == 3);
        ui.stop();
    }

    std::printf("test_plexui_flipack: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
