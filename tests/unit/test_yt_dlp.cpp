// Local yt-dlp: release choice, one resolve at a time, timeout/RSS kill,
// checksummed atomic update against 127.0.0.1 only.
#include "gcast/cast_resolve.hpp"
#include "gcast/dash_seek.hpp"
#include "gcast/yt_dlp.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace misterplex::gcast;
using namespace misterplex::gcast::yt_dlp;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::string slurp(const std::string& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static void writeFile(const std::string& path, const std::string& body) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
}

static bool writeExec(const std::string& path, const std::string& body) {
    writeFile(path, body);
    return ::chmod(path.c_str(), 0755) == 0;
}

struct FakeRelease {
    int fd = -1;
    int port = 0;
    std::thread thr;
    std::atomic<bool> stop{false};
    std::string sums;
    std::string body;
    int delayMs = 0;
    void start() {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = 0;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        CHECK(::listen(fd, 8) == 0);
        socklen_t l = sizeof a;
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &l);
        port = ntohs(a.sin_port);
        thr = std::thread([this] { loop(); });
    }
    void loop() {
        while (!stop.load()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            timeval tv{0, 100000};
            if (::select(fd + 1, &rfds, nullptr, nullptr, &tv) <= 0)
                continue;
            const int c = ::accept(fd, nullptr, nullptr);
            if (c < 0)
                continue;
            char buf[1024];
            std::string req;
            while (req.find("\r\n\r\n") == std::string::npos) {
                const ssize_t n = ::recv(c, buf, sizeof buf, 0);
                if (n <= 0)
                    break;
                req.append(buf, buf + n);
                if (req.size() > 8000)
                    break;
            }
            const bool sumsReq = req.find("GET /SHA2-256SUMS") != std::string::npos;
            const std::string& payload = sumsReq ? sums : body;
            if (delayMs > 0 && !sumsReq)
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            std::ostringstream hdr;
            hdr << "HTTP/1.0 200 OK\r\nContent-Length: " << payload.size() << "\r\nConnection: close\r\n\r\n";
            const std::string h = hdr.str();
            ::send(c, h.data(), h.size(), MSG_NOSIGNAL);
            ::send(c, payload.data(), payload.size(), MSG_NOSIGNAL);
            ::close(c);
        }
    }
    void shutdownSrv() {
        stop.store(true);
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            fd = -1;
        }
        if (thr.joinable())
            thr.join();
    }
};

int main() {
    const std::string empty = sha256Hex(nullptr, 0);
    CHECK(empty == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const uint8_t abc[] = {'a', 'b', 'c'};
    CHECK(sha256Hex(abc, 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(kStockMiSTerRelease == ReleaseKind::StandaloneArmv7l);
    CHECK(chooseRelease(3, 9, true, true) == ReleaseKind::StandaloneArmv7l);
    CHECK(chooseRelease(3, 9, false, true) == ReleaseKind::None);
    CHECK(chooseRelease(3, 10, false, true) == ReleaseKind::Zipapp);
    CHECK(chooseRelease(3, 10, true, true) == ReleaseKind::StandaloneArmv7l);
    CHECK(!pythonMeetsZipapp(3, 9) && pythonMeetsZipapp(3, 10));

    auto loc = planYouTube("aqz-KE-bpKQ", false, "", "", 624, 480, 2000, 0, true);
    CHECK(loc.route == Route::Direct && loc.playable == "yt-local:aqz-KE-bpKQ");
    CHECK(planYouTube("aqz-KE-bpKQ", false, "", "", 1, 1, 1, 0, false).route == Route::Refuse);
    GcastRouteInput in;
    in.url = "yt:aqz-KE-bpKQ";
    in.fpga = false;
    in.localYtdlp = true;
    CHECK(planGcastPlay(in).route == Route::Direct);

    char dir[] = "/tmp/yt-dlp-test-XXXXXX";
    CHECK(::mkdtemp(dir) != nullptr);
    const std::string root = dir;
    const std::string trace = root + "/trace";
    const std::string slow = root + "/slow.sh";
    CHECK(writeExec(slow, "#!/bin/sh\necho S $$ >> \"$YT_TRACE\"\nsleep 0.3\necho E $$ >> \"$YT_TRACE\"\n"
                          "echo https://example.invalid/v.mp4\n"));
    ::setenv("YT_TRACE", trace.c_str(), 1);
    ResolveOptions opt;
    opt.bin = slow;
    opt.videoId = "aqz-KE-bpKQ";
    opt.timeoutMs = 5000;
    opt.rssCapKb = 1024u * 1024u;
    const auto t0 = std::chrono::steady_clock::now();
    ResolveResult a, b;
    std::thread t1([&] { a = resolveVideo(opt); });
    std::thread t2([&] { b = resolveVideo(opt); });
    t1.join();
    t2.join();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(a.ok && b.ok);
    CHECK(ms >= 500);
    const std::string tr = slurp(trace);
    const auto s1 = tr.find("S ");
    const auto e1 = tr.find("E ");
    const auto s2 = tr.find("S ", s1 == std::string::npos ? 0 : s1 + 2);
    CHECK(s1 != std::string::npos && e1 != std::string::npos && s2 != std::string::npos && e1 < s2);

    const std::string hang = root + "/hang.sh";
    const std::string pidf = root + "/pid";
    CHECK(writeExec(hang, "#!/bin/sh\necho $$ > \"$YT_PID\"\nsleep 30\n"));
    ::setenv("YT_PID", pidf.c_str(), 1);
    ResolveOptions killOpt;
    killOpt.bin = hang;
    killOpt.videoId = "aqz-KE-bpKQ";
    killOpt.timeoutMs = 400;
    killOpt.rssCapKb = 1024u * 1024u;
    const ResolveResult timed = resolveVideo(killOpt);
    CHECK(!timed.ok && timed.timedOut);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const std::string pidTxt = slurp(pidf);
    if (!pidTxt.empty()) {
        const int pid = std::atoi(pidTxt.c_str());
        CHECK(pid > 1 && ::kill(pid, 0) != 0);
    }

    ::unlink(pidf.c_str());
    ResolveOptions rssOpt = killOpt;
    rssOpt.timeoutMs = 5000;
    rssOpt.rssCapKb = 1;
    const ResolveResult capped = resolveVideo(rssOpt);
    CHECK(!capped.ok && capped.rssKilled);
    const std::string pid2 = slurp(pidf);
    if (!pid2.empty()) {
        const int pid = std::atoi(pid2.c_str());
        CHECK(pid > 1 && ::kill(pid, 0) != 0);
    }

    const std::string bad = root + "/extract.sh";
    CHECK(writeExec(bad, "#!/bin/sh\necho 'ERROR: Unable to extract player response' >&2\nexit 1\n"));
    ResolveOptions ex;
    ex.bin = bad;
    ex.videoId = "aqz-KE-bpKQ";
    ex.timeoutMs = 2000;
    ex.rssCapKb = 1024u * 1024u;
    const ResolveResult er = resolveVideo(ex);
    CHECK(!er.ok && er.extractionError && !er.timedOut);

    const std::string pair = root + "/pair.sh";
    CHECK(writeExec(pair, "#!/bin/sh\necho https://example.invalid/v.mp4\necho https://example.invalid/a.m4a\n"));
    ResolveOptions two;
    two.bin = pair;
    two.videoId = "aqz-KE-bpKQ";
    two.timeoutMs = 2000;
    two.rssCapKb = 1024u * 1024u;
    const ResolveResult dash = resolveVideo(two);
    CHECK(dash.ok && dash.audioUrl == "https://example.invalid/a.m4a" &&
          dash.url == "https://example.invalid/v.mp4" && dash.url.find("bytes=") == std::string::npos &&
          dash.audioUrl.find("bytes=") == std::string::npos);

    // Progressive H.264/AAC is absent (a lab MiSTer gate: aqz-KE-bpKQ, YE7VzlLtp-4).
    // The same child answers only when -f also offers the AVC+AAC DASH pair.
    const std::string fmtf = root + "/fmt";
    ::setenv("YT_FMT", fmtf.c_str(), 1);
    const std::string dashOnly = root + "/dash_only.sh";
    CHECK(writeExec(dashOnly,
                    "#!/bin/sh\n"
                    "fmt=\nprev=\n"
                    "for a in \"$@\"; do\n"
                    "  if [ \"$prev\" = \"-f\" ]; then fmt=$a; fi\n"
                    "  prev=$a\n"
                    "done\n"
                    "printf '%s\\n' \"$fmt\" > \"$YT_FMT\"\n"
                    "dash='bv*[height<=720][vcodec^=avc][fps<=30]'\n"
                    "case \"$fmt\" in\n"
                    "  *\"$dash\"*)\n"
                    "    printf '%s\\n' 'https://example.invalid/dash-v.mp4'\n"
                    "    printf '%s\\n' 'https://example.invalid/dash-a.m4a'\n"
                    "    exit 0\n"
                    "    ;;\n"
                    "esac\n"
                    "echo 'ERROR: Requested format is not available' >&2\n"
                    "exit 1\n"));
    const char progOnly[] =
        "b[height<=720][vcodec^=avc][acodec^=mp4a][fps<=30][protocol=https]"
        "/b[height<=720][ext=mp4][fps<=30][protocol=https]"
        "/b[height<=720][fps<=30][protocol=https]";
    ResolveOptions oldSel;
    oldSel.bin = dashOnly;
    oldSel.videoId = "aqz-KE-bpKQ";
    oldSel.timeoutMs = 2000;
    oldSel.rssCapKb = 1024u * 1024u;
    oldSel.extraArgs = {"-f", progOnly, "-g", "--no-playlist", "--no-warnings",
                        "https://www.youtube.com/watch?v=aqz-KE-bpKQ"};
    const ResolveResult progGone = resolveVideo(oldSel);
    CHECK(!progGone.ok);
    CHECK(progGone.error.find("Requested format is not available") != std::string::npos);
    CHECK(std::string(kResolveFormat).compare(0, sizeof progOnly - 1, progOnly) == 0);
    CHECK(std::string(kResolveFormat).find(
              "/bv*[height<=720][vcodec^=avc][fps<=30][protocol=https]+ba[acodec^=mp4a][protocol=https]") !=
          std::string::npos);
    {   // every DASH alternative caps fps (720p60 AVC is offered first for 60 fps uploads)
        const std::string f(kResolveFormat);
        size_t at = 0, n = 0, capped = 0;
        while ((at = f.find("bv*", at)) != std::string::npos) {
            const size_t end = f.find('+', at);
            ++n;
            if (end != std::string::npos && f.substr(at, end - at).find("[fps<=30]") != std::string::npos)
                ++capped;
            at += 3;
        }
        CHECK(n >= 2 && capped == n);
    }
    {   // every progressive b[ option caps fps the same way the DASH bv* options do
        const std::string f(kResolveFormat);
        size_t at = 0, n = 0, capped = 0;
        while (at < f.size()) {
            if (f.compare(at, 2, "b[") != 0) {
                ++at;
                continue;
            }
            const size_t end = f.find('/', at);
            const std::string opt = f.substr(at, end == std::string::npos ? std::string::npos : end - at);
            ++n;
            if (opt.find("[fps<=30]") != std::string::npos)
                ++capped;
            if (end == std::string::npos)
                break;
            at = end + 1;
        }
        CHECK(n >= 3 && capped == n);
    }
    {   // every selector, including each progressive b[...] option and each DASH side, refuses HLS
        const std::string f(kResolveFormat);
        size_t at = 0, selectors = 0, guarded = 0;
        while (at <= f.size()) {
            const size_t slash = f.find('/', at);
            const std::string opt = f.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
            size_t side = 0;
            while (side <= opt.size()) {
                const size_t plus = opt.find('+', side);
                const std::string sel = opt.substr(side, plus == std::string::npos ? std::string::npos : plus - side);
                if (!sel.empty()) {
                    ++selectors;
                    if (sel.find("[protocol=https]") != std::string::npos ||
                        sel.find("[protocol!*=m3u8]") != std::string::npos)
                        ++guarded;
                }
                if (plus == std::string::npos)
                    break;
                side = plus + 1;
            }
            if (slash == std::string::npos)
                break;
            at = slash + 1;
        }
        CHECK(selectors >= 5 && guarded == selectors);
    }

    ::unlink(fmtf.c_str());
    ResolveOptions neu;
    neu.bin = dashOnly;
    neu.videoId = "YE7VzlLtp-4";
    neu.timeoutMs = 2000;
    neu.rssCapKb = 1024u * 1024u;
    const ResolveResult got = resolveVideo(neu);
    CHECK(got.ok && got.url == "https://example.invalid/dash-v.mp4" &&
          got.audioUrl == "https://example.invalid/dash-a.m4a");
    CHECK(slurp(fmtf) == std::string(kResolveFormat) + "\n");
    std::string fedV, fedA;
    CHECK(splitYtAv(ytAvPlayable(got.url, got.audioUrl), fedV, fedA));
    CHECK(fedV == got.url && fedA == got.audioUrl);

    FakeRelease srv;
    srv.body = "NEWBYTES";
    srv.sums = sha256Hex(reinterpret_cast<const uint8_t*>(srv.body.data()), srv.body.size()) + "  yt-dlp_linux_armv7l\n";
    srv.start();
    CHECK(srv.port > 0);
    const std::string dest = root + "/yt-dlp_linux_armv7l";
    writeFile(dest, "OLD");
    UpdateConfig cfg;
    cfg.baseUrl = "http://127.0.0.1:" + std::to_string(srv.port);
    cfg.asset = "yt-dlp_linux_armv7l";
    cfg.dest = dest;
    cfg.stampPath = root + "/stamp";
    cfg.intervalSec = 86400;
    const UpdateResult up = runVerifiedUpdate(cfg);
    CHECK(up.ok && up.swapped);
    CHECK(slurp(dest) == "NEWBYTES");
    CHECK(slurp(dest + ".prev") == "OLD");
    std::string rerr;
    CHECK(restorePrevious(dest, rerr));
    CHECK(slurp(dest) == "OLD");
    CHECK(slurp(dest + ".prev") == "NEWBYTES");

    writeFile(dest, "KEEP");
    srv.body = "TAMPER";
    const UpdateResult badUp = runVerifiedUpdate(cfg);
    CHECK(!badUp.ok && badUp.error.find("checksum") != std::string::npos);
    CHECK(slurp(dest) == "KEEP");

    srv.delayMs = 800;
    srv.body = "SLOW";
    srv.sums = sha256Hex(reinterpret_cast<const uint8_t*>(srv.body.data()), srv.body.size()) + "  yt-dlp_linux_armv7l\n";
    const std::string dest2 = root + "/slow-bin";
    cfg.dest = dest2;
    cfg.stampPath = root + "/stamp-slow";
    const auto u0 = std::chrono::steady_clock::now();
    CHECK(scheduleDailyUpdate(cfg));
    const auto uMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - u0).count();
    CHECK(uMs < 400);
    CHECK(!scheduleDailyUpdate(cfg));
    for (int i = 0; i < 40 && slurp(dest2) != "SLOW"; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(slurp(dest2) == "SLOW");
    CHECK(noteResolveFailure(er, cfg) == false);

    const std::string zip = root + "/onedir.zip";
    const std::string zipCmd =
        "python3 -c 'import zipfile; z=zipfile.ZipFile(\"" + zip +
        "\",\"w\"); z.writestr(\"yt-dlp_linux_armv7l\", b\"ZIPBYTES\"); z.writestr(\"_internal/m\", b\"i\")'";
    CHECK(std::system(zipCmd.c_str()) == 0);
    const std::string tree = root + "/onedir";
    std::string zerr;
    CHECK(installOnedirZip(zip, tree, zerr));
    CHECK(slurp(tree + "/yt-dlp_linux_armv7l") == "ZIPBYTES");
    CHECK(slurp(tree + "/_internal/m") == "i");
    CHECK(slurp(tree + "/yt-dlp").find("yt-dlp_linux_armv7l") != std::string::npos);
    const std::string launcher = root + "/launch.sh";
    CHECK(writeExec(launcher, "#!/bin/sh\nexec /bin/true\n"));
    const char* one[] = {launcher.c_str(), nullptr};
    CHECK(findBinary(3, 9, one).kind == ReleaseKind::StandaloneArmv7l);

    const std::string hpp = slurp("arm/misterplexd/gcast/yt_dlp.hpp");
    CHECK(hpp.find("--cacert") != std::string::npos);
    CHECK(hpp.find("caBundlePath") != std::string::npos);
    CHECK(hpp.find("yt-dlp_linux_armv7l.zip") != std::string::npos);

    srv.shutdownSrv();
    std::printf("test_yt_dlp: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
