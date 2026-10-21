// Prod line: seekTo keeps the playMedia server token (a lab MiSTer Quantum Leap scrub, mpxd-seek-1556: each seekTo replaced
// the remote server's transient token with Plex Web's controller X-Plex-Token -> 401 re-resolve, black screen,
// /:/timeline 401). Same companion rule on prod (v8 INT 52442f44d): the token hook took X-Plex-Token from seekTo.
//   S1 castControlRefreshToken: playMedia token= / X-Plex-Token install; seekTo / playback/* never install the
//      controller's X-Plex-Token (query or header); polls never do; an explicit token= still does.
//   S2 companion end to end over HTTP: the token hook never sees CONTROLLER; seek requests carry SERVER.
// Mutant: MUT_SEEK_TAKES_CONTROLLER_TOKEN dies at S1. (No seek coalescing on prod: that is High-only.)
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define private public
#include "companion.hpp"
#undef private
#include "playback_controls.hpp"

using namespace misterplex;
namespace {
int fails = 0;
void expect(bool c, const std::string& m) {
    if (!c) { std::fprintf(stderr, "FAIL seek token: %s\n", m.c_str()); ++fails; }
}

// ---- loopback companion host ----------------------------------------------------------------------
struct Host {
    Companion companion;
    std::atomic<uint64_t> play{0}, seek{0};
    Host() {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        socklen_t n = sizeof(a);
        getsockname(fd, reinterpret_cast<sockaddr*>(&a), &n);
        listen(fd, 16);
        companion.port_ = ntohs(a.sin_port);
        companion.httpListenFd_ = fd;
        companion.running_.store(true);
        companion.setMachineId("misterplex-dev");
        companion.setPlayQueued([this] { return ++play; });
    }
    void start() { companion.httpThr_ = std::thread([this] { companion.httpLoop(); }); }
    ~Host() { companion.stop(); }
    void get(const std::string& path, const std::string& extra = {}) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        timeval tv{5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(companion.port_);
        if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { close(fd); return; }
        const std::string req = "GET " + path + " HTTP/1.0\r\nHost: localhost\r\n" + extra + "Connection: close\r\n\r\n";
        send(fd, req.data(), req.size(), MSG_NOSIGNAL);
        shutdown(fd, SHUT_WR);
        char b[4096];
        while (recv(fd, b, sizeof(b), 0) > 0) {}
        close(fd);
    }
};

const char* kPlay = "/player/playback/playMedia?key=%2Flibrary%2Fmetadata%2F53653&offset=0&machineIdentifier=srv"
                    "&address=203.0.113.47&port=32401&protocol=http&token=SERVER&commandID=1";
}  // namespace

int main() {
    // ---- S1
    {
        const std::string hdr = "\r\nX-Plex-Token: CONTROLLER\r\n";
        expect(castControlRefreshToken("GET " + std::string(kPlay) + " HTTP/1.1") == "SERVER", "S1 playMedia token=");
        expect(castControlRefreshToken("GET /player/playback/playMedia?key=k HTTP/1.1" + hdr) == "CONTROLLER",
               "S1 playMedia header token (no token=) still installs");
        expect(castControlRefreshToken("GET /player/playback/seekTo?offset=354000&commandID=5 HTTP/1.1" + hdr).empty(),
               "S1 seekTo never installs the controller's header X-Plex-Token");
        expect(castControlRefreshToken("GET /player/playback/seekTo?offset=1&X-Plex-Token=CONTROLLER HTTP/1.1").empty(),
               "S1 seekTo never installs a query X-Plex-Token");
        expect(castControlRefreshToken("GET /player/playback/pause?commandID=6 HTTP/1.1" + hdr).empty(),
               "S1 playback/pause never installs it");
        expect(castControlRefreshToken("GET /player/timeline/poll?wait=1&token=X HTTP/1.1").empty(), "S1 polls never install");
        expect(castControlRefreshToken("GET /player/playback/seekTo?offset=1&token=SERVER2 HTTP/1.1") == "SERVER2",
               "S1 an explicit token= on a control still installs");
    }
    // ---- S2
    {
        Host host;
        std::vector<std::string> updates;
        std::vector<TransportRequest> seeks;
        std::mutex mu;
        host.companion.setTokenUpdate([&](const std::string& t) { std::lock_guard<std::mutex> l(mu); updates.push_back(t); });
        host.companion.setTransportQueued([&](TransportCommand c) {
            TransportGenerations g{host.play, host.seek};
            return g.accept(c, 1);
        });
        host.companion.setSeek([&](const TransportRequest& r) { std::lock_guard<std::mutex> l(mu); seeks.push_back(r); });
        host.start();
        host.get(kPlay);
        PlayRequest req;
        req.key = "/library/metadata/53653";
        req.token = "SERVER";   // main binds the resolved play (lastPlay), which carries the playMedia token
        host.companion.bindMedia(req, 2891936);
        host.companion.setState("playing", 120000, 2891936);
        host.get("/player/playback/seekTo?offset=354000&commandID=2", "X-Plex-Token: CONTROLLER\r\n");
        host.get("/player/playback/seekTo?offset=322000&commandID=3&X-Plex-Token=CONTROLLER");
        std::lock_guard<std::mutex> l(mu);
        expect(std::find(updates.begin(), updates.end(), "CONTROLLER") == updates.end(),
               "S2 the token hook never saw the controller token");
        expect(seeks.size() == 2, "S2 both seeks dispatched (" + std::to_string(seeks.size()) + ")");
        for (const auto& s : seeks)
            expect(s.media.token == "SERVER", "S2 the seek replays with the playMedia server token, got '" + s.media.token + "'");
    }
    std::printf("test_seek_token: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
