// owenb_idle_card: the "daemon not running" card, with no daemon (colorbar removal 10-23).
//   owenb_idle_card [--card down]      resident: arm the idle session, paint DAEMON NOT RUNNING, hold it
//                                      (exits 0 at once when a painter already runs)
//   owenb_idle_card --release          stop a resident painter (the pidfile), wait for it to close the session
// The daemon calls release first thing in main (the ring lock admits one feeder), so the painter yields to the
// daemon's own "Ready to cast" card. Links the owenb ring/session/overlay only: no media decoder, no network, and it
// never writes video_mode. SIGTERM/SIGINT close the ring session and exit 0.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "libmisterplex/idle_pairing_error.hpp"
#include "libmisterplex/owenb_idle_card_arm.hpp"
#include "libmisterplex/owenb_idle_card_ctl.hpp"
#include "libmisterplex/owenb_idle_card_fb.hpp"
#include "libmisterplex/owenb_play.hpp"
#include "libmisterplex/service_switch.hpp"

#ifndef MPX_DEVMEM_PATH
#define MPX_DEVMEM_PATH "/dev/mem"
#endif

using namespace misterplex;
namespace ctl = misterplex::owenb_idle_card;

static volatile std::sig_atomic_t g_stop = 0;
static void onSig(int) { g_stop = 1; }

static int64_t nowMs() {
    return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()).count());
}

static std::unique_ptr<owenb::OwenbDdrWindow> openWindow(std::string& err) {
    auto w = std::make_unique<owenb::DevMemDdrWindow>();
    const int rc = w->open(MPX_DEVMEM_PATH);
    if (rc != 0) {
        err = rc == -EBUSY ? "another program holds the owenb ring lock" : std::strerror(-rc);
        return nullptr;
    }
    return w;
}

static std::unique_ptr<owenb::IMediaSource> noSource(const owenb::OpenRequest&, std::string& err) {
    err = "owenb_idle_card plays nothing";
    return nullptr;
}

int main(int argc, char** argv) {
    bool releaseOnly = false;
    std::string card = "down";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--release"))
            releaseOnly = true;
        else if (!std::strcmp(argv[i], "--card") && i + 1 < argc)
            card = argv[++i];
        else {
            std::fprintf(stderr, "usage: owenb_idle_card [--card down] | --release\n");
            return 2;
        }
    }
    if (card != "down") {
        std::fprintf(stderr, "owenb_idle_card: only --card down exists\n");
        return 2;
    }
    const std::string pidfile = ctl::pidPath();
    if (releaseOnly)
        return ctl::release(pidfile, 3000) == 0 ? 0 : 1;
    if (ctl::isPainter(ctl::readPid(pidfile)))   // one painter at a time: the card is already up
        return 0;
    if (!ctl::writePid(pidfile, ::getpid())) {
        std::fprintf(stderr, "owenb_idle_card: cannot write %s\n", pidfile.c_str());
        return 1;
    }
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = onSig;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    owenb::OwenbPlayer player(openWindow, noSource);
    const char* spelunkr = std::getenv("SPELUNKR");
    player.ui().setSpelunkrMode(spelunkr && std::strcmp(spelunkr, "1") == 0);
    const auto initialState = service::readState();
    player.ui().setServiceNotice(initialState.overlay(), initialState.phase == "error");
    // The overlay renderer needs fb0: without this OsdConfig startIdle() armed the session and painted NOTHING
    // (PR #30 glass: flat black). armCard() also refuses to report "armed" while the renderer is not running.
    player.setOsd(ctl::fbOsdConfig());
    owenb::AspectConfig noAspect;   // the painter never touches the user's Analog Aspect (it has no store to restore it)
    noAspect.enabled = false;
    player.setAspect(noAspect);
    owenb::IdleConfig cfg;
    cfg.log = [](const std::string& l) { std::fprintf(stderr, "owenb_idle_card: %s\n", l.c_str()); };
    bool armed = false, failLogged = false;
    while (!g_stop) {
        const auto state = service::readState();
        const auto model = player.ui().snapshot();
        if (model.serviceNotice != state.overlay() || model.serviceFailed != (state.phase == "error"))
            player.ui().setServiceNotice(state.overlay(), state.phase == "error");
        if (!armed) {   // the core may still be coming up (boot) or the daemon may hold the ring: retry
            std::string err;
            if (ctl::armCard(player, cfg, nowMs(), err)) {
                armed = true;
                std::fprintf(stderr, "owenb_idle_card: armed: %s (overlay renderer running)\n", kIdleDaemonDownText);
            } else if (!failLogged) {
                failLogged = true;
                std::fprintf(stderr, "owenb_idle_card: not armed yet: %s\n", err.c_str());
            }
        }
        for (int i = 0; i < 20 && !g_stop; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    player.stopIdle();
    if (ctl::readPid(pidfile) == ::getpid())
        ::unlink(pidfile.c_str());
    return 0;
}
