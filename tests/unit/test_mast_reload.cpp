// MAST-WEDGE (review 21:14; release 21:16/21:17): host/libmisterplex/mast_reload.hpp.
//  - probeLine: live, lane-less, never-decoded and stuck probes each say which they are.
//  - the marker round-trips, and an unreadable one fails closed;
//  - decide: one reload per episode. A wedge after an unrecovered reload is a clean fail, and so
//    is one within kReloadMinIntervalS of a recovery; nothing happens without a launcher.
//  - spawnHelper: the launcher runs detached in a new session with only fds 0-2, never the
//    daemon's DDR fd, its flock'ed lock or its listening socket (the LOADCORE-GUARD
//    0-misterplexd rule). A missing launcher reports the exec error.
// Built with -DMUT_MAST_RELOAD_LOOP or -DMUT_MAST_KEEP_FDS, this test must FAIL.
#include "libmisterplex/mast_reload.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <thread>

using namespace misterplex::mast;

namespace {

int fails = 0;

void expect(bool ok, const std::string& what) {
    if (!ok) {
        ++fails;
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
    }
}

bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

int main() {
    // 1. Probe lines.
    {
        Probe p;
        expect(has(probeLine(p, "gate open"), "no MrAudio session lane"), "lane-less probe line");
        p.lane = true;
        p.mapped = true;
        p.reads = 1200;
        p.elapsedMs = 251;
        const std::string never = probeLine(p, "gate open");
        expect(has(never, "WEDGED: no valid MAST") && has(never, "never published"),
               "never-decoded probe line: " + never);
        p.decoded = 1200;
        p.firstPub = p.lastPub = 7;
        const std::string stuck = probeLine(p, "gate open");
        expect(has(stuck, "WEDGED: publication stuck at 7") && has(stuck, "+0 of 3"),
               "stuck probe line: " + stuck);
        p.live = true;
        p.lastPub = 10;
        p.fresh = 3;
        p.elapsedMs = 1;
        expect(has(probeLine(p, "gate open"), "live, publication 7 -> 10 (+3 in 1 ms)"),
               "live probe line");
        Probe unmapped;
        unmapped.lane = true;
        expect(has(probeLine(unmapped, "gate open"), "MAST unreadable"), "unmapped probe line");
    }
    // 2. Marker round trip; a file without a state fails closed.
    char dir[] = "/tmp/test_mast_reload.XXXXXX";
    expect(mkdtemp(dir) != nullptr, "mkdtemp");
    const std::string marker = std::string(dir) + "/marker";
    {
        expect(!readMarker(marker.c_str()).present, "absent marker reads as absent");
        expect(writeMarker("requested", 1000, "gate-open probe", marker.c_str()), "write marker");
        const Marker m = readMarker(marker.c_str());
        expect(m.present && m.state == "requested" && m.timeS == 1000 &&
                   m.reason == "gate-open probe",
               "marker round trip");
        std::ofstream(marker) << "garbage\n";
        const Marker g = readMarker(marker.c_str());
        expect(g.present && g.state.empty(), "garbage marker: present, no state");
        expect(decide(true, g, 5000) == Action::AfterReload, "garbage marker fails closed");
    }
    // 3. decide.
    {
        Marker none;
        expect(decide(false, none, 0) == Action::NoHelper, "no launcher: NoHelper");
        expect(decide(true, none, 0) == Action::RequestReload, "first wedge: RequestReload");
        Marker req = parseMarker("state=requested\ntime=100\nreason=begin\n");
        expect(decide(true, req, 101) == Action::NoReloadRan,
               "requested (hand-off not taken): NoReloadRan, never a new request (review 23:53 R2)");
        Marker att = parseMarker("state=attempted\ntime=100\nreason=begin\n");
        expect(decide(true, att, 100000) == Action::AfterReload,
               "attempted, even long ago: AfterReload (only a passed probe clears it)");
        Marker rec = parseMarker("state=recovered\ntime=100\nreason=begin\n");
        expect(decide(true, rec, 100 + kReloadMinIntervalS - 1) == Action::RateLimited,
               "recovered within the interval: RateLimited");
        expect(decide(true, rec, 100 + kReloadMinIntervalS) == Action::RequestReload,
               "recovered past the interval: a new episode may reload");
        expect(has(actionLine(Action::AfterReload, att, 160, "begin"), "no further reload") &&
                   has(actionLine(Action::AfterReload, att, 160, "begin"), "60 s ago"),
               "AfterReload line");
        expect(has(actionLine(Action::NoHelper, none, 0, "begin"), "MPX_MAST_RELOAD_CMD unset"),
               "NoHelper line");
        // review 21:39: a wedge while requested/attempted is the failed cure (terminal state,
        // one loud line); cure-failed itself stays a clean fail.
        expect(!cureFailed(req) && cureFailed(att) && !cureFailed(rec) && !cureFailed(none),
               "cureFailed: attempted only (requested = hand-off not taken, review 23:53 R2)");
        const std::string nt = actionLine(Action::NoReloadRan, req, 130, "gate-open probe");
        expect(has(nt, "no reload ran (hand-off not taken; marker requested 30 s ago)") &&
                   !has(nt, "CURE FAILED"),
               "requested wedge line: " + nt);
        // review 23:53 R2: a live probe writes recovered only from attempted.
        expect(onLive(att) == LiveAction::MarkRecovered, "live + attempted: recovered");
        expect(onLive(req) == LiveAction::HandoffNotTaken, "live + requested: hand-off not taken");
        for (const char* s : {"refused", "load-failed", ""}) {
            Marker t = parseMarker(std::string("state=") + s + "\ntime=100\nreason=begin\n");
            expect(onLive(t) == LiveAction::KeepTerminal,
                   std::string("live + ") + s + ": the terminal state stays");
        }
        // review 01:10 R5c: a gate opening on cure-failed is a manual recovery, never recovered.
        {
            Marker cfm = parseMarker(
                "state=cure-failed\ntime=100\nreason=gate-open probe\nwhy=gate-silent:x\n");
            expect(onLive(cfm) == LiveAction::ManualRecovery, "live + cure-failed: manual recovery");
            const std::string mr = liveLine(LiveAction::ManualRecovery, cfm, 130);
            expect(has(mr, "MAST lane live: manual recovery after cure-failed (marker cure-failed 30 s ago, gate-silent:x)") &&
                       has(mr, "stays cure-failed") && !has(mr, "marker now recovered") &&
                       !has(mr, "after the auto core reload"),
                   "manual-recovery line: " + mr);
        }
        const std::string lr = liveLine(LiveAction::MarkRecovered, att, 130);
        const std::string ln = liveLine(LiveAction::HandoffNotTaken, req, 130);
        expect(has(lr, "live after the auto core reload") && has(lr, "marker now recovered"),
               "recovered line: " + lr);
        expect(has(ln, "hand-off not taken: no reload ran (marker requested 30 s ago") &&
                   !has(ln, "after the auto core reload") && !has(ln, "recovered"),
               "hand-off-not-taken line: " + ln);
        Marker cf = parseMarker("state=cure-failed\ntime=100\nreason=begin\n");
        expect(!cureFailed(cf) && decide(true, cf, 100000) == Action::AfterReload,
               "cure-failed: terminal, never another reload");
        const std::string loud = cureFailedLine(att, 130, "gate-open probe");
        expect(has(loud, "MAST-WEDGE CURE FAILED (gate-open probe)") && has(loud, "30 s ago") &&
                   has(loud, "marker now cure-failed"),
               "cure-failed line: " + loud);
        // Lab force (glass rows): once only while no marker exists; always every time.
        expect(!forceWedge(nullptr, false) && !forceWedge("", false) && !forceWedge("yes", false),
               "force: off unless once/always");
        expect(forceWedge("once", false) && !forceWedge("once", true), "force once: no marker only");
        expect(forceWedge("always", false) && forceWedge("always", true), "force always");
        // review 21:52 M2 / review 21:52: a launcher that ran no load is terminal and says why.
        Marker ref = parseMarker(
            "state=refused\ntime=100\nreason=gate-open probe\nwhy=core-switched:MENU\n");
        expect(ref.why == "core-switched:MENU", "marker why parses");
        expect(decide(true, ref, 100000) == Action::NoReloadRan, "refused: NoReloadRan, terminal");
        expect(!cureFailed(ref), "refused is no failed cure (no reload ran)");
        const std::string nr = actionLine(Action::NoReloadRan, ref, 130, "begin");
        expect(has(nr, "no reload ran (core-switched:MENU; marker refused 30 s ago)") &&
                   has(nr, "no auto reload until the MiSTer reboots"),
               "NoReloadRan line: " + nr);
        // review 22:00 N1: only the guard's rc 97/98 is a refusal; any other loader rc means a
        // reload was attempted past the guard and failed (state=load-failed): terminal, said so.
        Marker grd = parseMarker("state=refused\ntime=100\nreason=begin\nwhy=guard-rc97\n");
        expect(decide(true, grd, 100000) == Action::NoReloadRan, "guard refusal: NoReloadRan");
        Marker lf = parseMarker(
            "state=load-failed\ntime=100\nreason=gate-open probe\nwhy=load-failed-rc1\n");
        expect(decide(true, lf, 100000) == Action::LoadFailed, "load-failed: LoadFailed, terminal");
        expect(!cureFailed(lf), "load-failed is no failed cure (the load itself failed)");
        expect(loaderRc(lf) == "1" && loaderRc(ref) == "?" && loaderRc(grd) == "?",
               "loader rc from why=load-failed-rc<rc> only");
        const std::string lfl = actionLine(Action::LoadFailed, lf, 130, "begin");
        expect(has(lfl, "reload attempted, loader rc=1 (load-failed-rc1; marker load-failed 30 s ago)") &&
                   has(lfl, "no auto reload until the MiSTer reboots") && !has(lfl, "no reload ran"),
               "LoadFailed line: " + lfl);
        // R5a (review 01:09, review 01:10/01:28): attempted + a gate that never opens, after
        // max(kGateDeadlineS, the 5-probe give-up), with CORENAME Plex: terminal cure-failed.
        {
            const int64_t B = kGateDeadlineS;
            expect(B == 60, "the product bound is 60 s");
            expect(silentGateCureFailed(att, false, false, true, B), "silent gate: cure-failed at the bound");
            expect(!silentGateCureFailed(att, false, false, true, B - 1), "before the bound: wait");
            expect(!silentGateCureFailed(att, true, false, true, B + 100), "an open gate: no verdict");
            expect(!silentGateCureFailed(att, false, true, true, B + 100),
                   "the gate still probing: wait for its 5-probe give-up");
            expect(!silentGateCureFailed(att, false, false, false, B + 100),
                   "CORENAME not Plex (a user switch): no failed cure");
            for (const char* s : {"requested", "recovered", "cure-failed", "refused", "load-failed"}) {
                Marker t = parseMarker(std::string("state=") + s + "\ntime=100\nreason=begin\n");
                expect(!silentGateCureFailed(t, false, false, true, B + 100),
                       std::string("silent gate on ") + s + ": no verdict (attempted only)");
            }
            expect(!silentGateCureFailed(Marker{}, false, false, true, B + 100), "no marker: no verdict");
            const std::string sl = silentGateLine(att, 190, 61, "no CAPS reply after 5 probes");
            expect(sl.rfind("MAST cure failed: the core did not answer CAPS after the reload (no CAPS reply after 5 probes; 61 s since this daemon started, bound max(60 s, the gate's 5-probe give-up)", 0) == 0 &&
                       has(sl, "marker attempted 90 s ago") && has(sl, "(marker now cure-failed)") &&
                       has(sl, "; R6 before the reload: not recorded)"),
                   "silent-gate line: " + sl);
            // R6 (review 02:08): the loader's r6= line parses, is quoted, and survives rewrites.
            Marker a6 = parseMarker("state=attempted\ntime=100\nreason=gate-open probe\n"
                                    "r6=not-quiesced (consumed != written within 3000 ms: x)\n");
            expect(a6.state == "attempted" && a6.r6 == "not-quiesced (consumed != written within 3000 ms: x)",
                   "marker r6 parses");
            expect(has(silentGateLine(a6, 190, 61, "g"),
                       "; R6 before the reload: not-quiesced (consumed != written within 3000 ms: x))"),
                   "silent-gate line quotes r6");
            expect(formatMarker("cure-failed", 5, "r", "w", "quiesced (q)") ==
                       "state=cure-failed\ntime=5\nreason=r\nwhy=w\nr6=quiesced (q)\n" &&
                       formatMarker("recovered", 5, "r", "", "quiesced (q)") ==
                           "state=recovered\ntime=5\nreason=r\nr6=quiesced (q)\n" &&
                       formatMarker("requested", 5, "r") == "state=requested\ntime=5\nreason=r\n",
                   "formatMarker: why and r6 only when set");
            const std::string mk6 = std::string(dir) + "/r6.marker";
            expect(writeMarkerWhy("cure-failed", 190, a6.reason, "gate-silent:g", mk6.c_str(), a6.r6) &&
                       readMarker(mk6.c_str()).r6 == a6.r6 && readMarker(mk6.c_str()).why == "gate-silent:g",
                   "r6 survives the cure-failed rewrite");
            // the marker carries why=gate-silent:<reason> (review 01:28 (1): the marker is the status)
            const std::string mk = std::string(dir) + "/silent.marker";
            expect(writeMarkerWhy("cure-failed", 190, "gate-open probe", "gate-silent:no CAPS reply after 5 probes", mk.c_str()),
                   "write marker with why");
            const Marker back = readMarker(mk.c_str());
            expect(back.state == "cure-failed" && back.why == "gate-silent:no CAPS reply after 5 probes" &&
                       back.reason == "gate-open probe" && back.timeS == 190,
                   "marker why round-trips");
            // the launcher's loud channel: '<date> up=<s> !!! <text>' and '<date> <text>'
            const std::string bl = std::string(dir) + "/boot.log", ff = std::string(dir) + "/boot.FAILED";
            appendBootLoud("MAST cure failed: test", bl.c_str(), ff.c_str());
            std::ifstream b1(bl), f1(ff);
            std::string l1, l2;
            std::getline(b1, l1);
            std::getline(f1, l2);
            expect(has(l1, " up=") && has(l1, " !!! MAST cure failed: test"), "boot log loud line: " + l1);
            expect(has(l2, " MAST cure failed: test") && !has(l2, "!!!"), "FAILED file line: " + l2);
        }
    }
    // 4. spawnHelper: detached, new session, only fds 0-2 (no DDR fd, lock or socket).
    {
        const std::string ddr = std::string(dir) + "/fake_ddr";
        const std::string lock = std::string(dir) + "/misterplexd.lock";
        const std::string out = std::string(dir) + "/helper.out";
        const std::string helper = std::string(dir) + "/helper.sh";
        std::ofstream(ddr) << "ddr";
        const int ddrFd = open(ddr.c_str(), O_RDWR);
        const int lockFd = open(lock.c_str(), O_RDWR | O_CREAT, 0644);
        expect(ddrFd >= 0 && lockFd >= 0 && flock(lockFd, LOCK_EX | LOCK_NB) == 0,
               "fixture fds");
        const int sock = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        expect(sock >= 0 && bind(sock, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 &&
                   listen(sock, 1) == 0,
               "fixture listening socket");
        {
            std::ofstream h(helper);
            h << "#!/bin/sh\n"
              << "sleep 1\n"
              << "{ echo \"argv=$*\"; echo \"sid=$(cut -d' ' -f6 /proc/$$/stat) "
                 "ppid=$(cut -d' ' -f4 /proc/$$/stat)\";\n"
              << "  ls -l /proc/$$/fd; } > " << out << ".tmp\n"
              << "mv " << out << ".tmp " << out << "\n";
        }
        chmod(helper.c_str(), 0755);
        struct stat sockSt {};
        fstat(sock, &sockSt);
        const std::string sockName = "socket:[" + std::to_string(sockSt.st_ino) + "]";
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        const bool started = spawnHelper(helper, getpid(), "gate-open probe", marker, &err);
        const auto spawnMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
        expect(started, "spawnHelper: " + err);
        // The exec report pipe is CLOEXEC: spawnHelper must not wait for the launcher (which
        // itself waits for the daemon to exit).
        expect(spawnMs < 700, "spawnHelper blocked " + std::to_string(spawnMs) +
                                  " ms on a launcher that runs 1 s");
        std::string rec;
        for (int i = 0; i < 100 && (rec = slurp(out)).empty(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        expect(has(rec, "argv=mast-reload " + std::to_string(getpid()) + " gate-open probe " +
                            marker),
               "launcher argv: " + rec.substr(0, rec.find('\n')));
        int sid = -1, ppid = -1;
        const size_t s = rec.find("sid=");
        if (s != std::string::npos)
            std::sscanf(rec.c_str() + s, "sid=%d ppid=%d", &sid, &ppid);
        expect(sid > 0 && sid != getsid(0) && ppid != getpid(),
               "launcher detached: sid " + std::to_string(sid) + " (ours " +
                   std::to_string(getsid(0)) + "), ppid " + std::to_string(ppid));
        expect(!has(rec, ddr), "launcher inherited the DDR fd");
        expect(!has(rec, lock), "launcher inherited the lock fd");
        expect(!has(rec, sockName), "launcher inherited the listening socket " + sockName);
        close(sock);
        close(lockFd);
        close(ddrFd);
        std::string err2;
        expect(!spawnHelper(std::string(dir) + "/missing-launcher", getpid(), "begin", marker,
                            &err2) &&
                   has(err2, "exec"),
               "a missing launcher is reported (" + err2 + ")");
    }
    if (fails) {
        std::printf("FAIL MAST reload hand-off (%d failures)\n", fails);
        return 1;
    }
    std::printf("PASS MAST reload hand-off (probe lines, marker, decide table, detached launcher "
                "with fds 0-2 only, missing launcher)\n");
    return 0;
}
