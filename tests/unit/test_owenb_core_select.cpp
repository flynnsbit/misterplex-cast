#include "libmisterplex/core_id_select.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int fails = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++fails;
    }
}

misterplex::OwenbCore coreFor(const char* mode) {
    misterplex::OwenbCore out = misterplex::OwenbCore::PlexOB;
    check(misterplex::owenbCoreForOutput(mode, out), mode);
    return out;
}

} // namespace

int main() {
    const auto i480 = coreFor("480i");
    const auto p480 = coreFor("480p");
    const auto p240 = coreFor("240p");
    check(i480 == misterplex::OwenbCore::PlexOB && p480 == misterplex::OwenbCore::PlexOB,
          "480i and 480p share PlexOB");
    check(!misterplex::owenbNeedsLoadCore(i480, p480), "480i to 480p load_core");
    check(!misterplex::owenbNeedsLoadCore(p480, i480), "480p to 480i load_core");
    check(p240 == misterplex::OwenbCore::PlexOB240p, "240p is PlexOB_240p");
    check(misterplex::owenbNeedsLoadCore(p480, p240), "480p to 240p load_core");

    check(std::strcmp(misterplex::owenbUtilityRbf(i480), "/media/fat/_Utility/PlexOB.rbf") == 0,
          "PlexOB rbf");
    check(std::strcmp(misterplex::owenbUtilityRbf(p240), "/media/fat/_Utility/PlexOB_240p.rbf") == 0,
          "PlexOB_240p rbf");
    check(misterplex::loadCoreLineAllowed(misterplex::owenbLoadCoreLine(i480)), "PlexOB line");
    check(misterplex::loadCoreLineAllowed(misterplex::owenbLoadCoreLine(p240)), "PlexOB_240p line");
    check(misterplex::loadCoreLineAllowed("load_core /media/fat/_Utility/Plex_480p.rbf"),
          "legacy 480p line");
    check(!misterplex::loadCoreLineAllowed("load_core /media/fat/_Utility/Plex.rbf"),
          "generic Plex.rbf");
    auto prod = [](const std::string& p) { return p == "/media/fat/PlexOB.rbf"; };
    auto util = [](const std::string& p) { return p == "/media/fat/_Utility/PlexOB.rbf"; };
    auto none = [](const std::string&) { return false; };
    auto yes = [](const std::string&) { return true; };
    check(misterplex::plexObReloadLoadCoreLine("PlexOB", prod) == "load_core /media/fat/PlexOB.rbf",
          "name-only PlexOB resolves to /media/fat/PlexOB.rbf when that file exists");
    check(misterplex::plexObReloadLoadCoreLine("PlexOB", util) ==
              "load_core /media/fat/_Utility/PlexOB.rbf",
          "name-only PlexOB falls back to the utility RBF when prod is absent");
    check(misterplex::plexObReloadLoadCoreLine("PlexOB", none).empty(),
          "name-only PlexOB refuses the bounce when no RBF exists");
    check(misterplex::plexObReloadLoadCoreLine("/media/fat/PlexOB.rbf", yes) ==
              "load_core /media/fat/PlexOB.rbf",
          "loaded /media/fat/PlexOB.rbf is reloaded as itself");
    check(misterplex::plexObReloadLoadCoreLine("/media/fat/PlexOB.rbf", none).empty(),
          "a missing full path is refused before the bounce");
    check(misterplex::loadCoreLineAllowed("load_core /media/fat/PlexOB.rbf"),
          "prod PlexOB.rbf is allow-listed");
    check(misterplex::plexObReloadLoadCoreLine("/media/fat/_Utility/PlexOB_480p.rbf", yes) ==
              "load_core /media/fat/_Utility/PlexOB_480p.rbf",
          "a loaded 480p path is reloaded as itself");
    check(misterplex::plexObReloadLoadCoreLine("PlexOB_480p", none).empty(),
          "name-only PlexOB_480p is not the hard-coded utility default");
    check(!misterplex::loadCoreLineAllowed("load_core /tmp/PlexOB.rbf"),
          "paths outside /media/fat are refused");
    check(misterplex::strandedOnMenuCore("Menu") && misterplex::strandedOnMenuCore("MENU"),
          "Menu is the stranded core");
    check(!misterplex::strandedOnMenuCore("PlexOB") && !misterplex::strandedOnMenuCore("Plex"),
          "PlexOB and legacy Plex are not the menu gap");
    check(misterplex::menuBounceRecoveryDue("Menu", true),
          "a bounce marker plus Menu is recovered");
    check(!misterplex::menuBounceRecoveryDue("Menu", false),
          "a deliberate menu visit has no marker and is left alone");
    check(misterplex::bareDaemonMenuReload("Menu"),
          "a bare daemon start on Menu reloads PlexOB");
    check(!misterplex::bareDaemonMenuReload("PlexOB"),
          "a bare daemon on PlexOB does not reload");
    check(!misterplex::menuBounceRecoveryDue("PlexOB", true),
          "a marker on PlexOB is not a menu gap");
    check(misterplex::staleMenuBounceMarker("PlexOB", true),
          "a leftover marker on PlexOB is cleared at startup");
    check(misterplex::staleMenuBounceMarker("Plex", true),
          "a leftover marker on legacy Plex is cleared at startup");
    check(!misterplex::staleMenuBounceMarker("Menu", true),
          "a marker on Menu is the recovery gap");
    check(!misterplex::staleMenuBounceMarker("PlexOB", false),
          "no marker is not a leftover");
    {
        std::ifstream in(std::string(MPX_SOURCE_ROOT) + "/arm/misterplexd/media_player.cpp");
        std::stringstream buf;
        buf << in.rdbuf();
        const std::string src = buf.str();
        const auto fn = src.find("MediaPlayer::recoverStrandedMenuCore");
        const auto next = src.find("MediaPlayer::reloadCoreForYcMode", fn == std::string::npos ? 0 : fn);
        const auto stale = fn == std::string::npos ? std::string::npos : src.find("staleMenuBounceMarker", fn);
        const auto due = fn == std::string::npos ? std::string::npos : src.find("menuBounceRecoveryDue", fn);
        const auto clr = stale == std::string::npos ? std::string::npos : src.find("clearMenuBounceMarker", stale);
        check(in.good() && fn != std::string::npos && stale != std::string::npos && due != std::string::npos &&
                  clr != std::string::npos && next != std::string::npos && stale < clr && clr < due && due < next,
              "startup recovery clears a leftover marker before the Menu check");
        check(src.find("bareDaemonMenuReload") != std::string::npos,
              "startup recovery uses bareDaemonMenuReload");
        std::ifstream mainIn(std::string(MPX_SOURCE_ROOT) + "/arm/misterplexd/main.cpp");
        std::stringstream mainBuf;
        mainBuf << mainIn.rdbuf();
        check(mainBuf.str().find("recoverStrandedMenuCore(true)") != std::string::npos,
              "process start calls recoverStrandedMenuCore(true)");
    }
    {
        const char* marker = "/tmp/mpx-yc-menu-bounce-test";
        misterplex::clearMenuBounceMarker(marker);
        check(!misterplex::menuBounceMarkerPresent(marker), "marker starts absent");
        check(misterplex::writeMenuBounceMarker(marker), "marker write");
        check(misterplex::menuBounceMarkerPresent(marker), "marker is a regular file");
        check(misterplex::menuBounceRecoveryDue("Menu", misterplex::menuBounceMarkerPresent(marker)),
              "written marker recovers Menu");
        misterplex::clearMenuBounceMarker(marker);
        check(!misterplex::menuBounceMarkerPresent(marker), "marker cleared after PlexOB loads");
    }
    check(!misterplex::plexObReloadWriteMenu(true, false),
          "INT before the menu load does not start a bounce");
    check(misterplex::plexObReloadWriteMenu(false, false), "a live reload may write menu.rbf");
    check(misterplex::plexObReloadWriteCore(true, true),
          "INT after menu.rbf still writes the PlexOB load_core");
    check(!misterplex::plexObReloadWriteCore(true, false),
          "INT with the menu core not loaded does not write PlexOB yet");

    misterplex::OwenbCore unused = misterplex::OwenbCore::PlexOB;
    check(!misterplex::owenbCoreForOutput("720p", unused), "720p is a PlexOB output");

    if (fails) {
        std::fprintf(stderr, "test_owenb_core_select: %d FAIL\n", fails);
        return 1;
    }
    std::puts("test_owenb_core_select: OK");
    return 0;
}
