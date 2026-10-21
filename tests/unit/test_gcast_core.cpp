#include <cstdlib>
// GCAST P1/P2 core: SessionArbiter, cast_resolve, airplay_state (host, no UxPlay).
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "gcast/airplay_state.hpp"
#include "gcast/cast_resolve.hpp"
#include "gcast/session_arbiter.hpp"

using namespace misterplex::gcast;
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::string slurp(const char* path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void arbiter() {
    SessionArbiter a;
    std::vector<std::string> log;
    a.setRetire(Owner::Plex, [&](Owner by) { log.push_back(std::string("retire Plex by ") + ownerName(by)); });
    a.setRetire(Owner::AirPlay, [&](Owner by) { log.push_back(std::string("retire AirPlay by ") + ownerName(by)); });
    int apProg = 0;
    a.setProgress(Owner::AirPlay, [&](const std::string&, int64_t, int64_t) { ++apProg; });
    CHECK(a.owner() == Owner::None);
    CHECK(!a.dispatchProgress("playing", 0, 0));          // None -> caller's Plex path
    const uint64_t p1 = a.claim(Owner::Plex);
    CHECK(log.empty());                                      // nobody to retire
    CHECK(!a.dispatchProgress("playing", 1, 2));           // Plex -> caller
    const uint64_t ap = a.claim(Owner::AirPlay);
    CHECK(log.size() == 1 && log[0] == "retire Plex by AirPlay");
    CHECK(a.dispatchProgress("playing", 1, 2) && apProg == 1);
    CHECK(!a.release(Owner::Plex, p1));                    // not the owner
    const uint64_t ap2 = a.claim(Owner::AirPlay);          // same owner, new session: no retire
    CHECK(log.size() == 1);
    CHECK(!a.release(Owner::AirPlay, ap));                 // stale token
    CHECK(a.owner() == Owner::AirPlay);
    CHECK(a.current(Owner::AirPlay, ap2) && !a.current(Owner::AirPlay, ap));
    a.claim(Owner::Plex);                                    // Plex preempts AirPlay
    CHECK(log.size() == 2 && log[1] == "retire AirPlay by Plex");
    CHECK(!a.dispatchProgress("playing", 3, 4) && apProg == 1);
    const uint64_t y = a.claim(Owner::YouTube);
    CHECK(log.size() == 3);
    CHECK(a.release(Owner::YouTube, y) && a.owner() == Owner::None);
}

static void resolve() {
    CHECK(urlPathExt("http://h:1/a/b/movie.MP4?x=1.mkv") == "mp4");
    CHECK(urlPathExt("http://h.example.com/path/noext") == "");
    CHECK(urlPathExt("http://h.example.com") == "");
    CHECK(encodeComponent("a b&c=d/\xc3\xa9") == "a%20b%26c%3Dd%2F%C3%A9");
    auto d = planCast("http://192.0.2.24:18990/test.mp4", true, "http://x:1", 1280, 720, 3000, 0);
    CHECK(d.route == Route::Direct && d.playable == "http://192.0.2.24:18990/test.mp4");
    auto s = planCast("https://cdn/x.mp4", true, "http://lab-pc:18780/", 1280, 720, 3000, 5000);
    CHECK(s.route == Route::Helper);
    CHECK(s.playable == "http://lab-pc:18780/ladder.ts?src=https%3A%2F%2Fcdn%2Fx.mp4&w=1280&h=720&br=3000&gcast=1&offset_ms=5000");
    auto hls = planCast("http://h/master.m3u8", true, "http://lab-pc:18780", 640, 480, 1500, 0);
    CHECK(hls.route == Route::Helper && hls.playable.find("offset_ms") == std::string::npos);
    auto nd = planCast("http://h/a.mp4", false, "http://lab-pc:18780", 640, 480, 1500, 0);
    CHECK(nd.route == Route::Helper);                       // direct off -> helper
    auto nh = planCast("https://h/a.m3u8", true, "", 640, 480, 1500, 0);
    CHECK(nh.route == Route::Refuse && nh.why.find("gcast-xcode") != std::string::npos);
    CHECK(planCast("rtsp://h/a.mp4", true, "http://x", 1, 1, 1, 0).route == Route::Refuse);
    CHECK(planCast("mlhls://localhost/master.m3u8", true, "http://x", 1, 1, 1, 0).route == Route::Refuse);
    CHECK(planCast("http://h/a.mp4\r\nX: y", true, "http://x", 1, 1, 1, 0).route == Route::Refuse);
    CHECK(planCast("HTTP://H/A.MKV", true, "", 1, 1, 1, 0).route == Route::Direct);
    CHECK(planCast("https://h/a.mp4", true, "", 1, 1, 1, 0).route == Route::Refuse);   // no https in libav
    // YouTube (yt:<id>) only via the helper; the id is validated.
    std::string id;
    CHECK(ytCastId("yt:aqz-KE-bpKQ", id) && id == "aqz-KE-bpKQ");
    CHECK(!ytCastId("yt:aqz-KE-bpK", id) && !ytCastId("yt:aqz-KE-bpKQQ", id) && !ytCastId("xx:aqz-KE-bpKQ", id));
    CHECK(!ytCastId("yt:aqz&KE-bpKQ", id) && !ytCastId("yt:aqz/KE-bpKQ", id) && !ytCastId("yt:aqz KE-bpKQ", id));
    auto y = planCast("yt:aqz-KE-bpKQ", true, "http://lab-pc:18780", 624, 480, 2000, 7000);
    CHECK(y.route == Route::Helper);
    CHECK(y.playable == "http://lab-pc:18780/ladder.ts?src=yt%3Aaqz-KE-bpKQ&w=624&h=480&br=2000&gcast=1&offset_ms=7000");
    CHECK(planCast("yt:aqz-KE-bpKQ", true, "", 1, 1, 1, 0).route == Route::Refuse);
    CHECK(planCast("yt:aqz-KE-bp?Q", true, "http://x", 1, 1, 1, 0).route == Route::Refuse);
    CHECK(ytRemuxSource("http://lab-pc:18780/", "aqz-KE-bpKQ", 12000) == "http://lab-pc:18780/yt.ts?v=aqz-KE-bpKQ&start_ms=12000");
    CHECK(ytRemuxSource("http://lab-pc:18780", "aqz-KE-bpKQ", -5) == "http://lab-pc:18780/yt.ts?v=aqz-KE-bpKQ&start_ms=0");
    // Standalone: MPX_GCAST_LOCAL supplies helper when GCAST_XCODE unset.
    setenv("MPX_GCAST_LOCAL", "http://127.0.0.1:18781", 1);
    unsetenv("GCAST_XCODE");
    CHECK(standaloneLocalBase() == "http://127.0.0.1:18781");
    CHECK(effectiveXcodeBase(false) == "http://127.0.0.1:18781");
    CHECK(effectiveXcodeBase(true) == "http://127.0.0.1:18781");  // no remote
    setenv("GCAST_XCODE", "http://helper:18780", 1);
    CHECK(effectiveXcodeBase(true) == "http://helper:18780");
    CHECK(effectiveXcodeBase(false) == "http://127.0.0.1:18781");  // fall back local
    auto ys = planCast("yt:aqz-KE-bpKQ", true, standaloneLocalBase(), 624, 480, 2000, 0);
    CHECK(ys.route == Route::Helper && ys.playable.find("127.0.0.1:18781") != std::string::npos);
    unsetenv("MPX_GCAST_LOCAL");
    unsetenv("GCAST_XCODE");
    // A yt-dlp path on disk is not the helper. /health must answer first.
    CHECK(effectiveXcodeBase(false, false).empty());
    CHECK(standaloneLocalBase(false).empty());
    CHECK(effectiveXcodeBase(false, true) == "http://127.0.0.1:18781");
    auto healthy = planYouTube("aqz-KE-bpKQ", false, "", effectiveXcodeBase(false, true), 624, 480, 2000, 0, true);
    CHECK(healthy.route == Route::Direct &&
          healthy.playable == "http://127.0.0.1:18781/yt.ts?v=aqz-KE-bpKQ&start_ms=0");
    auto down = planYouTube("aqz-KE-bpKQ", false, "", effectiveXcodeBase(false, false), 624, 480, 2000, 0, true);
    CHECK(down.route == Route::Direct && down.playable == "yt-local:aqz-KE-bpKQ");
    // B2: a healthy on-card helper must not take YouTube when local yt-dlp is present.
    const std::string loopback = effectiveXcodeBase(false, true);
    CHECK(loopback == "http://127.0.0.1:18781");
    CHECK(youtubeXcodeBase(false, true, loopback).empty());
    auto oncard = planYouTube("aqz-KE-bpKQ", false, "", youtubeXcodeBase(false, true, loopback),
                               624, 480, 2000, 0, true);
    CHECK(oncard.route == Route::Direct && oncard.playable == "yt-local:aqz-KE-bpKQ");
    CHECK(youtubeXcodeBase(true, true, "http://192.0.2.10:18780") == "http://192.0.2.10:18780");
    const std::string castResolve = slurp("arm/misterplexd/gcast/cast_resolve.hpp");
    CHECK(castResolve.find("probeHttpHealth") != std::string::npos);
    CHECK(castResolve.find("/health") != std::string::npos);
    CHECK(httpStatusLineIs200("HTTP/1.0 200 OK\r\n\r\n"));
    CHECK(httpStatusLineIs200("HTTP/1.1 200 OK\n"));
    CHECK(!httpStatusLineIs200("HTTP/1.1 500 Internal\r\nX-Note: 200\r\n\r\n"));
    CHECK(!httpStatusLineIs200("HTTP/1.1 2000 OK\r\n"));
    CHECK(!httpStatusLineIs200("not http 200"));
    const std::string mainHealth = slurp("arm/misterplexd/main.cpp");
    CHECK(mainHealth.find("cachedLoopbackHelperHealth") != std::string::npos);
    CHECK(mainHealth.find("youtubeXcodeBase") != std::string::npos);
    CHECK(mainHealth.find("cachedHelperProbeSelfTest") != std::string::npos);
    {
        const auto refused = mainHealth.find("GCAST youtube refused");
        CHECK(refused != std::string::npos);
        const auto from = refused > 240 ? refused - 240 : 0;
        const auto window = mainHealth.substr(from, 480);
        CHECK(window.find("offlineClockLogAllowed") == std::string::npos);
    }
    // MPX_LADDER_URL preferred over MPX_REMOTE_LADDER_PROXY
    setenv("MPX_LADDER_URL", "http://lab:18767", 1);
    setenv("MPX_REMOTE_LADDER_PROXY", "http://old:18767", 1);
    CHECK(fpgaLadderProxyFromEnv() == "http://lab:18767");
    unsetenv("MPX_LADDER_URL");
    CHECK(fpgaLadderProxyFromEnv() == "http://old:18767");
    unsetenv("MPX_REMOTE_LADDER_PROXY");
    CHECK(fpgaLadderProxyFromEnv().empty());
    // planYouTube: OB copy-remux Direct; legacy FPGA via ladder; legacy w/o ladder refuses; no xcode refuses.
    auto ob = planYouTube("aqz-KE-bpKQ", false, "", "http://127.0.0.1:18781", 624, 480, 2000, 5000);
    CHECK(ob.route == Route::Direct && ob.playable == "http://127.0.0.1:18781/yt.ts?v=aqz-KE-bpKQ&start_ms=5000");
    auto ob2 = planYouTube("aqz-KE-bpKQ", false, "http://lad:18767", "http://lab-pc:18780", 624, 480, 2000, 0);
    CHECK(ob2.route == Route::Direct && ob2.playable.find("/yt.ts?v=") != std::string::npos);
    auto lg = planYouTube("aqz-KE-bpKQ", true, "http://lad:18767", "http://127.0.0.1:18781", 624, 480, 2000, 9000);
    CHECK(lg.route == Route::Helper && lg.playable.rfind("http://lad:18767/ladder.ts?src=", 0) == 0 &&
          lg.playable.find("offset_ms") == std::string::npos && lg.playable.find("start_ms%3D9000") != std::string::npos);
    CHECK(planYouTube("aqz-KE-bpKQ", true, "", "http://127.0.0.1:18781", 1, 1, 1, 0).route == Route::Refuse);
    CHECK(planYouTube("aqz-KE-bpKQ", false, "", "", 1, 1, 1, 0).route == Route::Refuse);
    // xcode unset, local yt-dlp present, OB: direct local resolve (not the 18781 helper).
    auto loc = planYouTube("aqz-KE-bpKQ", false, "", "", 624, 480, 2000, 0, true);
    CHECK(loc.route == Route::Direct && loc.playable == "yt-local:aqz-KE-bpKQ");
    CHECK(loc.why.find("local yt-dlp") != std::string::npos);
    CHECK(planYouTube("aqz-KE-bpKQ", true, "", "", 1, 1, 1, 0, true).route == Route::Refuse);
    GcastRouteInput localIn;
    localIn.url = "yt:aqz-KE-bpKQ";
    localIn.fpga = false;
    localIn.localYtdlp = true;
    localIn.directWanted = true;
    auto localPlan = planGcastPlay(localIn);
    CHECK(localPlan.route == Route::Direct && localPlan.playable == "yt-local:aqz-KE-bpKQ");
    // planGcastPlay is what castPlay calls. Dropping a route here, or the call in main.cpp, fails.
    {
        std::ifstream in("arm/misterplexd/main.cpp");
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string main = ss.str();
        CHECK(main.find("planGcastPlay(") != std::string::npos);
        CHECK(main.find("planStartMs") == std::string::npos);
        CHECK(main.find("startWallClockRescue") != std::string::npos);
        CHECK(main.find("waitForWallClockRescue") != std::string::npos);
    }
    const std::string xcode = "http://127.0.0.1:18781";
    const std::string ladder = "http://127.0.0.1:18767";
    const std::string raop = "http://127.0.0.1:7000/raop.wav";
    GcastRouteInput ap;
    ap.url = raop;
    ap.fpga = true;
    ap.ladder = ladder;
    ap.xcode = xcode;
    ap.helper = ladder;
    ap.w = 624;
    ap.h = 480;
    ap.brKbps = 2000;
    ap.startMs = 4000;
    auto apPlan = planGcastPlay(ap);
    CHECK(apPlan.route == Route::Helper);
    CHECK(apPlan.playable.find("/ladder.ts?src=") != std::string::npos);
    CHECK(apPlan.playable.find("127.0.0.1") != std::string::npos);
    CHECK(apPlan.playable.find("raop.wav") != std::string::npos);
    CHECK(apPlan.playable.find("offset_ms") == std::string::npos);
    GcastRouteInput obyt;
    obyt.url = "yt:aqz-KE-bpKQ";
    obyt.fpga = false;
    obyt.xcode = xcode;
    obyt.helper = xcode;
    obyt.w = 624;
    obyt.h = 480;
    obyt.brKbps = 2000;
    obyt.startMs = 5000;
    obyt.directWanted = true;
    auto obPlan = planGcastPlay(obyt);
    CHECK(obPlan.route == Route::Direct);
    CHECK(obPlan.playable == "http://127.0.0.1:18781/yt.ts?v=aqz-KE-bpKQ&start_ms=5000");
    GcastRouteInput fpgayt = obyt;
    fpgayt.fpga = true;
    fpgayt.ladder = ladder;
    fpgayt.helper = ladder;
    fpgayt.startMs = 9000;
    auto fpgaPlan = planGcastPlay(fpgayt);
    CHECK(fpgaPlan.route == Route::Helper);
    CHECK(fpgaPlan.playable.rfind("http://127.0.0.1:18767/ladder.ts?src=", 0) == 0);
    CHECK(fpgaPlan.playable.find("start_ms%3D9000") != std::string::npos);
    CHECK(fpgaPlan.playable.find("127.0.0.1%3A18781") != std::string::npos);
    fpgayt.ladder.clear();
    fpgayt.helper.clear();
    CHECK(planGcastPlay(fpgayt).route == Route::Refuse);
    auto p = parseProbe("w=1920\nh=1080\r\ndur_ms=60000\nfps_n=30000\nfps_d=1001\naudio=1\nvcodec=hevc\njunk\n");
    CHECK(p.ok && p.w == 1920 && p.h == 1080 && p.durMs == 60000 && p.fpsN == 30000 && p.audio && p.vcodec == "hevc");
    CHECK(!parseProbe("w=-5\nh=99999\n").ok);
}

static void apstate() {
    EventQueue q;
    q.push({ApEventType::Play, "http://a", 0});
    q.push({ApEventType::Scrub, "", 10});
    q.push({ApEventType::Play, "http://b", 5});              // coalesces the old play + scrub
    CHECK(q.size() == 1);
    ApEvent e;
    CHECK(q.pop(e, 10) && e.type == ApEventType::Play && e.url == "http://b" && e.value == 5);
    CHECK(!q.pop(e, 5));
    for (int i = 0; i < 40; ++i) q.push({ApEventType::Rate, "", 1});
    CHECK(q.size() == EventQueue::kCap);                      // capped
    CHECK(q.push({ApEventType::Stop, "", 0}) && q.size() == EventQueue::kCap);

    PlaybackSnapshot s;
    PlaybackInfoOut o;
    s.info(o);
    CHECK(o.duration == 0 && o.position == -1);              // idle: not available
    s.loading(5);
    s.info(o);
    CHECK(o.position == 5 && o.rate == 0 && !o.ready);
    s.progress("playing", 7000, 60000);
    s.info(o);
    CHECK(o.duration == 60 && o.position == 7 && o.rate == 1 && o.ready);
    s.progress("paused", 8000, 0);
    s.info(o);
    CHECK(o.rate == 0 && o.ready && o.duration == 60);
    s.progress("ended", 60000, 60000);
    s.info(o);
    CHECK(o.duration == -1);                                  // end reported once
    s.info(o);
    CHECK(o.duration == 0 && o.position == -1);              // then idle
    s.progress("playing", 1000, 2000);                         // late progress after end ignored
    s.info(o);
    CHECK(o.position == -1);
    s.loading(0);
    s.ended();                                                 // preempted while loading
    s.info(o);
    CHECK(o.duration == -1);
}

// Headers, then 50 ms, then the body. A one-recv probe misses audio=1.
static void splitBodyProbe() {
    int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(lfd >= 0);
    int yes = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    CHECK(::listen(lfd, 1) == 0);
    sockaddr_in bound{};
    socklen_t bl = sizeof bound;
    CHECK(::getsockname(lfd, reinterpret_cast<sockaddr*>(&bound), &bl) == 0);
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        const int c = ::accept(lfd, nullptr, nullptr);
        if (c >= 0) {
            char tmp[512];
            ::recv(c, tmp, sizeof tmp, 0);
            const char* hdr = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n";
            ::send(c, hdr, std::strlen(hdr), MSG_NOSIGNAL);
            ::usleep(50000);
            const char* body = "w=640\nh=480\naudio=1\nvcodec=none\n";
            ::send(c, body, std::strlen(body), MSG_NOSIGNAL);
            ::close(c);
        }
        _exit(0);
    }
    const std::string base = "http://127.0.0.1:" + std::to_string(ntohs(bound.sin_port));
    CHECK(probeHttpHealth(base, 2000, "/probe?selftest=1", "audio=1"));
    ::close(lfd);
    int st = 0;
    ::waitpid(child, &st, 0);
}

int main() {
    arbiter();
    resolve();
    apstate();
    splitBodyProbe();
    std::printf("test_gcast_core: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
