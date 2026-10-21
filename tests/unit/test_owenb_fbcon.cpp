// OWENB-P1B 0009 (G15): fbcon is detached from the console while the OB overlay holds fb0, attached
// again at the release -- only what the guard detached -- and after a crash at the next start. Runs on a
// temp tree shaped like /sys/class/vtconsole; never touches /sys.
#include "libmisterplex/owenb_fbcon_guard.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

using misterplex::owenb::FbconGuard;

namespace {
int fails = 0, checks = 0;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(c)) {                                                                                                    \
            ++fails;                                                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                  \
            std::fprintf(stderr, __VA_ARGS__);                                                                         \
            std::fprintf(stderr, "\n");                                                                                \
        }                                                                                                              \
    } while (0)

struct Tree {
    std::string root;
    explicit Tree(const std::string& tag) {
        root = "/tmp/test_owenb_fbcon." + tag + "." + std::to_string(::getpid());
        if (std::system(("rm -rf " + root).c_str()) != 0) {}
        ::mkdir(root.c_str(), 0755);
        ::mkdir((root + "/vt").c_str(), 0755);
    }
    ~Tree() { if (std::system(("rm -rf " + root).c_str()) != 0) {} }
    void vt(int n, const char* name, const char* bind) {
        const std::string d = root + "/vt/vtcon" + std::to_string(n);
        ::mkdir(d.c_str(), 0755);
        FbconGuard::writeFile(d + "/name", std::string(name) + "\n");
        FbconGuard::writeFile(d + "/bind", std::string(bind) + "\n");
    }
    std::string bind(int n) const { return FbconGuard::readFile(root + "/vt/vtcon" + std::to_string(n) + "/bind"); }
    FbconGuard::Config cfg(FbconGuard::Policy p = FbconGuard::Policy::Unbind) const {
        FbconGuard::Config c;
        c.policy = p;
        c.vtRoot = root + "/vt";
        c.marker = root + "/marker";
        c.cmdline = root + "/cmdline";
        return c;
    }
    bool marker() const { return !FbconGuard::readFile(root + "/marker").empty(); }
};
} // namespace

int main() {
    {   // C1: MiSTer's layout: vtcon0 dummy, vtcon1 fbcon bound -> detached while held, attached at release
        Tree t("c1");
        t.vt(0, "(S) dummy device", "0");
        t.vt(1, "(M) frame buffer device", "1");
        std::vector<std::string> log;
        auto c = t.cfg();
        c.log = [&](const std::string& s) { log.push_back(s); };
        FbconGuard g(c);
        CHECK(g.scan().size() == 1 && g.scan()[0].bound, "C1 one fbcon vtcon found, bound");
        CHECK(g.acquire() == 1 && t.bind(1) == "0" && t.bind(0) == "0" && t.marker(),
              "C1 acquire: fbcon detached (bind %s), the dummy untouched, the marker written", t.bind(1).c_str());
        CHECK(g.acquire() == 1 && t.bind(1) == "0", "C1 a second acquire (the idle session, then a play): no change");
        CHECK(g.release() == 1 && t.bind(1) == "1" && !t.marker() && !g.held(),
              "C1 release: attached again (bind %s), the marker gone", t.bind(1).c_str());
        CHECK(g.release() == 0 && t.bind(1) == "1", "C1 a second release: nothing");
        CHECK(!log.empty(), "C1 logged");
    }
    {   // C2: the user had fbcon detached already: never attached by us
        Tree t("c2");
        t.vt(1, "(M) frame buffer device", "0");
        FbconGuard g(t.cfg());
        CHECK(g.acquire() == 0 && !t.marker(), "C2 nothing to detach, no marker");
        g.release();
        CHECK(t.bind(1) == "0", "C2 release leaves the user's detached console alone (bind %s)", t.bind(1).c_str());
    }
    {   // C3: MPX_OB_FBCON=keep: looked at, never written
        Tree t("c3");
        t.vt(1, "(M) frame buffer device", "1");
        FbconGuard g(t.cfg(FbconGuard::Policy::Keep));
        CHECK(g.acquire() == 0 && t.bind(1) == "1" && !t.marker(), "C3 keep: fbcon untouched");
        g.release();
        CHECK(t.bind(1) == "1", "C3 keep: still bound after the release");
    }
    {   // C4: a daemon died holding fb0: the next start attaches what the marker lists (only under vtRoot)
        Tree t("c4");
        t.vt(1, "(M) frame buffer device", "1");
        {
            FbconGuard dead(t.cfg());
            dead.acquire();
        }   // no release: the process "died"
        CHECK(t.bind(1) == "0" && t.marker(), "C4 the crash left fbcon detached, the marker in place");
        FbconGuard next(t.cfg());
        CHECK(next.restoreLeftover() == 1 && t.bind(1) == "1" && !t.marker(), "C4 the next start attached it again");
        FbconGuard::writeFile(t.root + "/marker", "/etc/passwd\n" + t.root + "/vt/vtcon1\n");
        FbconGuard again(t.cfg());
        CHECK(again.restoreLeftover() == 1, "C4 a marker line outside vtRoot is ignored");
        CHECK(next.restoreLeftover() == 0, "C4 no marker: nothing");
    }
    {   // C5: no vtconsole tree (no fbcon in the kernel); the cmdline check for an image already fixed
        Tree t("c5");
        auto c = t.cfg();
        c.vtRoot = t.root + "/absent";
        FbconGuard g(c);
        CHECK(g.acquire() == 0 && g.release() == 0, "C5 no fbcon: nothing done");
        FbconGuard::writeFile(t.root + "/cmdline", "console=ttyS0,115200 fbcon=map:1 loglevel=4\n");
        CHECK(g.imageMapsFbconAway(), "C5 fbcon=map:1 keeps fbcon off fb0");
        FbconGuard::writeFile(t.root + "/cmdline", "console=ttyS0 fbcon=map:01\n");
        CHECK(!g.imageMapsFbconAway(), "C5 fbcon=map:01 still maps a console to fb0");
        FbconGuard::writeFile(t.root + "/cmdline", "console=ttyS0\n");
        CHECK(!g.imageMapsFbconAway(), "C5 no fbcon=map: fbcon takes fb0");
    }
    if (fails) {
        std::fprintf(stderr, "test_owenb_fbcon: %d of %d checks FAIL\n", fails, checks);
        return 1;
    }
    std::printf("test_owenb_fbcon: OK (%d checks)\n", checks);
    return 0;
}
