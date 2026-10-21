// OSD Send Home (BrowseOp::Open) from a library grid lands on Home.
#include "plexui_controller.hpp"

#include <cstdio>

int main() {
    misterplex::plexui::PlexUiController::Hooks hooks;
    hooks.get = [](const std::string&, int, int, const misterplex::BrowseCancelFn&) {
        return misterplex::BrowseHttpResult{};
    };
    misterplex::plexui::PlexUiController ui(misterplex::plexui::PlexUiController::Config{"tk", ""}, hooks);
    misterplex::plexui::PlexServer server;
    server.name = "Box";
    server.baseUrl = "http://127.0.0.1:9";
    ui.testSetLibraryGrid(server, {}, {});
    if (ui.testView() != misterplex::plexui::ViewId::LibraryGrid) {
        std::printf("FAIL seed view\n");
        return 1;
    }
    ui.testHandleOpen();
    if (ui.testView() != misterplex::plexui::ViewId::Home) {
        std::printf("FAIL Send Home stayed off Home\n");
        return 1;
    }
    std::printf("test_plexui_send_home: ok\n");
    return 0;
}
