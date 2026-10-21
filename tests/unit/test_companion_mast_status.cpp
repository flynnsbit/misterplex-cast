// GET /misterplex/mast through the real companion HTTP loop (Companion::httpLoop on a loopback
// port), as it is wired in arm/misterplexd/companion.cpp (MAST R5b follow-up; review 01:28/03:52):
//  - status lines and type: 404 without a marker, 200 with one (text/plain), 405 for another
//    method, 400 with parameters;
//  - read-only: the route answers before any other handling, so a status request whose headers
//    carry a cast-control path and a token binds nothing and takes no token (a /resources request
//    is the positive control: it does bind);
//  - the request-latency log (companion_reqlog.hpp) covers the route: one line per request.
// Built with -DMPX_MAST_RELOAD_MARKER_PATH pointing at a file under build/.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#define private public
#include "companion.hpp"
#undef private
#include "libmisterplex/mast_reload.hpp"

namespace {

int fails = 0;
int rows = 0;

void expect(bool ok, const std::string& what) {
    ++rows;
    if (!ok) {
        ++fails;
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
    }
}

bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

struct Host {
    misterplex::Companion companion;
    std::mutex mu;
    std::vector<std::string> lines;

    Host() {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (fd < 0 || bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            std::perror("bind");
            std::exit(2);
        }
        socklen_t size = sizeof(address);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) != 0 ||
            listen(fd, 8) != 0) {
            std::perror("listen");
            std::exit(2);
        }
        companion.port_ = ntohs(address.sin_port);
        companion.httpListenFd_ = fd;
        companion.running_.store(true);
        companion.setLog([this](const std::string& s) {
            std::lock_guard<std::mutex> lock(mu);
            lines.push_back(s);
        });
        companion.httpThr_ = std::thread([this] { companion.httpLoop(); });
    }
    ~Host() { companion.stop(); }

    std::string send(const std::string& request) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        timeval timeout{3, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(companion.port_);
        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            close(fd);
            return "(connect failed)";
        }
        ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
        shutdown(fd, SHUT_WR);
        std::string response;
        char buffer[4096];
        ssize_t n;
        while ((n = recv(fd, buffer, sizeof(buffer), 0)) > 0)
            response.append(buffer, static_cast<size_t>(n));
        close(fd);
        return response;
    }
    size_t logged(const std::string& sub) {
        std::lock_guard<std::mutex> lock(mu);
        size_t n = 0;
        for (const auto& l : lines)
            n += has(l, sub) ? 1 : 0;
        return n;
    }
};

std::string body(const std::string& response) {
    const size_t p = response.find("\r\n\r\n");
    return p == std::string::npos ? std::string() : response.substr(p + 4);
}

}  // namespace

int main() {
    const char* marker = misterplex::mast::kMarkerPath;
    std::remove(marker);
    Host host;
    const std::string get = "GET /misterplex/mast HTTP/1.1\r\nHost: localhost\r\n\r\n";

    std::string r = host.send(get);
    expect(r.rfind("HTTP/1.1 404 Not Found\r\n", 0) == 0 && has(r, "Content-Type: text/plain\r\n") &&
               body(r) == "marker=absent\nstate=\ntime=0\nreason=\nwhy=\nr6=\ncure_failed=0\n",
           "no marker: 404 + absent body: " + r.substr(0, 40));

    {
        std::ofstream f(marker, std::ios::trunc);
        f << "state=cure-failed\ntime=1759650000\nreason=gate-open probe\n"
             "why=gate-silent:no CAPS reply after 5 probes\n";
    }
    r = host.send(get);
    expect(r.rfind("HTTP/1.1 200 OK\r\n", 0) == 0 && has(r, "Content-Type: text/plain\r\n") &&
               has(body(r), "marker=present\nstate=cure-failed\n") &&
               has(body(r), "\ncure_failed=1\n"),
           "cure-failed marker: 200 text/plain, cure_failed=1: " + r.substr(0, 40));

    r = host.send("POST /misterplex/mast HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n");
    expect(r.rfind("HTTP/1.1 405 Method Not Allowed\r\n", 0) == 0 && body(r) == "GET only\n",
           "POST: 405 Method Not Allowed: " + r.substr(0, 40));
    r = host.send("GET /misterplex/mast?X-Plex-Token=SECRETQ HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expect(r.rfind("HTTP/1.1 400 Bad Request\r\n", 0) == 0 && body(r) == "no parameters\n",
           "parameters: 400 Bad Request: " + r.substr(0, 40));

    // Read-only: the headers name a cast-control path and carry a token, as a crafted request
    // could; the status route answers first, so nothing binds and no token is taken.
    r = host.send("GET /misterplex/mast HTTP/1.1\r\nHost: localhost\r\n"
                  "Referer: http://x/player/playback/playMedia?token=STOLEN\r\n"
                  "X-Plex-Token: STOLEN\r\n\r\n");
    {
        std::lock_guard<std::mutex> lock(host.companion.mu_);
        expect(r.rfind("HTTP/1.1 200 OK\r\n", 0) == 0 && !host.companion.castBound_ &&
                   host.companion.pendingToken_.empty(),
               "status request binds nothing and takes no token");
    }
    r = host.send("GET /resources HTTP/1.1\r\nHost: localhost\r\n\r\n");
    {
        std::lock_guard<std::mutex> lock(host.companion.mu_);
        expect(r.rfind("HTTP/1.1 200 OK\r\n", 0) == 0 && host.companion.castBound_,
               "positive control: /resources binds the cast");
    }

    // The latency log covers the route on every outcome (one line per request; not folded).
    // ReqTimer logs after close(): wait for the last request's line.
    for (int i = 0; i < 200 && host.logged("path=GET /resources") == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    expect(host.logged("companion: req ") + host.logged("companion: WARN slow req ") >= 6,
           "the request log saw every request");
    expect(host.logged("path=GET /misterplex/mast hdr_ms=") == 4 &&
               host.logged("path=POST /misterplex/mast hdr_ms=") == 1,
           "the request log names the status route, the query never logged");
    expect(host.logged("STOLEN") == 0 && host.logged("SECRETQ") == 0, "no token in any log line");

    std::remove(marker);
    if (fails) {
        std::fprintf(stderr, "test_companion_mast_status: %d of %d checks FAILED\n", fails, rows);
        return 1;
    }
    std::printf("test_companion_mast_status: %d checks OK (real httpLoop: 404/200/405/400, "
                "text/plain, read-only before cast/token handling, request log covers the route)\n",
                rows);
    return 0;
}
