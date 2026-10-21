// Plex Web 4.162.1 cast-poller replay against the real companion httpLoop (loopback).
//
// Models the Plex Web client's companion manager ("remoteCastPlayerManager" in the
// app.plex.tv bundle main-8792...-plex-4.162.1): requestSession sets the command counter
// c=0 and polls GET /player/timeline/poll?commandID=c (then wait=1 forever);
// startPlayback sets a busy flag u, increments c and sends playMedia with commandID=c,
// clearing u when the playMedia XHR completes. A poll reply is APPLIED only when
// !u && int(MediaContainer@commandID) >= c and a video Timeline exists; applying it sets
// c to the reply's commandID and moves the scrubber to floor(time/1000).
//
// Rows:
//   R1 direct      : one controller id, commandIDs carried; every poll after playMedia must
//                    apply and the scrubber must follow setState progress.
//   R2 reload      : the same client id reached commandID 57 in an earlier page; the new
//                    page restarts at 0. Every reply must echo exactly the commandID that
//                    poll sent (never the old page's 57), and Plex Web still applies and advances.
//   R3 relay-shape : playMedia carries the browser id + commandID, but polls arrive with
//                    neither (identity-stripping relay). Such polls echo the active cast
//                    owner's commandID, so Plex Web applies them and the scrubber advances
//                    (before the owner floor every reply echoed 0 < c: frozen at the offset).
//   R4 relay-sub   : a subscriber that never sent a commandID (a relay's subscription)
//                    gets pushes carrying the owner's commandID (>= the browser's c).
//   R5 own-command : a controller that does send commandIDs keeps its own echo on polls
//                    and pushes (the owner floor only fills a missing commandID).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#define private public
#include "companion.hpp"
#undef private

namespace {

int fails = 0;

void expect(bool ok, const std::string& what) {
    if (!ok) {
        ++fails;
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
    }
}

std::string attr(const std::string& xml, const std::string& elemStart, const char* name) {
    const auto e = xml.find(elemStart);
    if (e == std::string::npos)
        return {};
    const auto end = xml.find('>', e);
    const std::string elem = xml.substr(e, end == std::string::npos ? std::string::npos : end - e);
    const std::string k = std::string(" ") + name + "=\"";
    const auto p = elem.find(k);
    if (p == std::string::npos)
        return {};
    const auto b = p + k.size();
    return elem.substr(b, elem.find('"', b) - b);
}

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
        companion.setMachineId("player-uuid");
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

    std::string get(const std::string& target, const std::string& headers) {
        const std::string request = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" +
                                    headers + "\r\n";
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        timeval timeout{3, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(companion.port_);
        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            close(fd);
            return {};
        }
        ::send(fd, request.data(), request.size(), MSG_NOSIGNAL);
        std::string response;
        char buffer[4096];
        ssize_t n;
        while ((n = recv(fd, buffer, sizeof(buffer), 0)) > 0)
            response.append(buffer, static_cast<size_t>(n));
        close(fd);
        const auto p = response.find("\r\n\r\n");
        return p == std::string::npos ? std::string() : response.substr(p + 4);
    }
};

// Plex Web 4.162.1 companion-manager state for one page (requestSession .. endSession).
struct PlexWeb {
    PlexWeb(Host& h, std::string id) : host(h), clientId(std::move(id)) {}
    Host& host;
    std::string clientId;  // X-Plex-Client-Identifier ("" = stripped by a relay)
    bool pollCarriesId = true;
    bool pollCarriesCommand = true;
    long c = 0;
    bool u = false;
    int polls = 0, applied = 0;
    long lastPositionS = -1;
    std::vector<std::string> replyCmds;

    std::string headers(bool withId) const {
        std::string h = "X-Plex-Target-Client-Identifier: player-uuid\r\n";
        if (withId && !clientId.empty())
            h += "X-Plex-Client-Identifier: " + clientId + "\r\n";
        return h;
    }
    void requestSession() { c = 0; }
    // Returns true when Plex Web would apply the reply (scrubber moves to its time).
    bool poll(bool wait) {
        std::string target = "/player/timeline/poll?";
        if (wait)
            target += "wait=1&";
        if (pollCarriesCommand)
            target += "commandID=" + std::to_string(c) + "&";
        target += "type=video";
        const std::string xml = host.get(target, headers(pollCarriesId));
        ++polls;
        const std::string cmd = attr(xml, "<MediaContainer", "commandID");
        replyCmds.push_back(cmd.empty() ? "(none)" : cmd);
        const long p = cmd.empty() ? -1 : std::strtol(cmd.c_str(), nullptr, 10);
        if (u || p < c)
            return false;
        const std::string state = attr(xml, "<Timeline type=\"video\"", "state");
        if (state.empty())
            return false;
        c = p;
        ++applied;
        lastPositionS = std::strtol(attr(xml, "<Timeline type=\"video\"", "time").c_str(), nullptr,
                                    10) / 1000;
        return true;
    }
    void playMedia(const std::string& key, long offsetMs) {
        u = true;
        ++c;
        const std::string target =
            "/player/playback/playMedia?type=video&providerIdentifier=com.plexapp.plugins.library"
            "&containerKey=%2FplayQueues%2F4242%3Fown%3D1&key=" + key +
            "&offset=" + std::to_string(offsetMs) +
            "&machineIdentifier=server-uuid&protocol=https&address=192-0-2-41.abc.plex.direct"
            "&port=32400&token=transient-test&commandID=" + std::to_string(c);
        host.get(target, headers(true));
        u = false;  // playMedia XHR completed (done or fail both clear it)
    }
};

void bindPlaying(Host& host, const std::string& key, long offsetMs, long durationMs) {
    misterplex::PlayRequest bound;
    bound.key = key;
    bound.containerKey = "/playQueues/4242?own=1";
    bound.playQueueId = "4242";
    bound.serverMachineId = "server-uuid";
    bound.protocol = "https";
    bound.address = "192-0-2-41.abc.plex.direct";
    bound.port = "32400";
    bound.offsetMs = offsetMs;
    bound.offsetPresent = true;
    expect(host.companion.bindMedia(bound, durationMs), "bindMedia rejected " + key);
}

void playAndFollow(Host& host, PlexWeb& web, const char* row, bool mustApply) {
    const std::string key = "/library/metadata/154219";
    const long offsetMs = 600000, durationMs = 5400000;
    web.playMedia(key, offsetMs);
    bindPlaying(host, key, offsetMs, durationMs);
    std::vector<long> seen;
    for (int i = 0; i < 5; ++i) {
        host.companion.setState("playing", offsetMs + 2000 * (i + 1), durationMs);
        const bool ok = web.poll(true);
        seen.push_back(ok ? web.lastPositionS : -1);
        if (mustApply)
            expect(ok, std::string(row) + ": poll " + std::to_string(i) + " not applied (c=" +
                           std::to_string(web.c) + " reply_cmd=" + web.replyCmds.back() + ")");
    }
    std::string positions, cmds;
    for (long s : seen)
        positions += (positions.empty() ? "" : ",") + std::to_string(s);
    for (size_t i = web.replyCmds.size() - 5; i < web.replyCmds.size(); ++i)
        cmds += (cmds.empty() ? "" : ",") + web.replyCmds[i];
    const bool advanced = seen.back() > offsetMs / 1000;
    if (mustApply)
        expect(advanced, std::string(row) + ": scrubber did not advance past the offset");
    std::printf("PLEXWEB_REPLAY row=%s applied=%d/%d c=%ld positions_s=[%s] reply_cmd=[%s] "
                "scrubber=%s\n",
                row, web.applied, web.polls, web.c, positions.c_str(), cmds.c_str(),
                advanced ? "ADVANCES" : "FROZEN");
}

}  // namespace

int main() {
    {
        Host host;
        PlexWeb web(host, "plexweb-tab-A");
        web.requestSession();
        web.poll(false);
        playAndFollow(host, web, "R1-direct", true);
        if (std::getenv("MPX_COMPANION_POLL_DIAG")) {
            std::lock_guard<std::mutex> lock(host.mu);
            for (const auto& line : host.lines)
                if (line.find("diag") != std::string::npos)
                    std::printf("R1 %s\n", line.c_str());
        }
    }
    {
        Host host;
        PlexWeb old(host, "plexweb-tab-B");
        old.requestSession();
        old.c = 57;
        old.poll(true);
        PlexWeb web(host, "plexweb-tab-B");
        web.requestSession();
        web.poll(false);
        playAndFollow(host, web, "R2-reload", true);
        bool exact = true;
        for (const auto& cmd : web.replyCmds)
            exact = exact && cmd != "57";
        expect(exact, "R2: a reloaded page got the previous page's commandID 57 back");
    }
    {
        Host host;
        PlexWeb old(host, "cp1011-plexweb");
        old.requestSession();
        host.companion.subscribeTimeline("cp1011-plexweb", "127.0.0.1", "http", 32600, "628");
        old.c = 629;
        old.poll(true);
        expect(old.replyCmds.back() == "629", "R6: old session echo " + old.replyCmds.back() + " != 629");
        PlexWeb web(host, "cp1011-plexweb");  // reload: same X-Plex-Client-Identifier, counter restarts
        web.requestSession();
        web.c = 1;
        web.poll(true);
        const std::string e1 = web.replyCmds.back();
        web.c = 2;
        web.poll(true);
        const std::string e2 = web.replyCmds.back();
        expect(e1 == "1", "R6: reload poll commandID=1 echoed " + e1 + " (want 1)");
        expect(e2 == "2", "R6: reload poll commandID=2 echoed " + e2 + " (want 2)");
        misterplex::Companion::TimelineSubscriber sub;
        {
            std::lock_guard<std::mutex> lock(host.companion.subscriberMu_);
            sub = host.companion.subscribers_.front();
        }
        const std::string pushed = host.companion.pushCommandFor(sub);
        expect(pushed == "2", "R6: subscriber push after the reload carries " + pushed + " (want 2)");
        web.pollCarriesCommand = false;
        web.poll(true);
        const std::string eNone = web.replyCmds.back();
        expect(eNone == "2", "R6: commandID-less poll after the reload echoed " + eNone + " (want 2)");
        host.companion.unsubscribeTimeline("cp1011-plexweb", "127.0.0.1");
        const std::string after = host.companion.timelineCommandFor("cp1011-plexweb", "127.0.0.1", "");
        expect(after == "0", "R6: after unsubscribe a commandID-less request echoed " + after + " (want 0)");
        std::printf("PLEXWEB_REPLAY row=R6-restart old=%s reload=[%s,%s] push=%s none=%s after_unsub=%s\n",
                    old.replyCmds.back().c_str(), e1.c_str(), e2.c_str(), pushed.c_str(), eNone.c_str(),
                    after.c_str());
    }
    {
        Host host;
        PlexWeb web(host, "plexweb-tab-C");
        web.requestSession();
        web.poll(false);
        web.pollCarriesId = false;
        web.pollCarriesCommand = false;
        playAndFollow(host, web, "R3-relay-shape", true);
    }
    {
        Host host;
        PlexWeb web(host, "plexweb-tab-D");
        web.requestSession();
        web.poll(false);
        host.companion.subscribeTimeline("", "192.0.2.41", "http", 32400, "");
        playAndFollow(host, web, "R4-relay-sub", true);
        misterplex::Companion::TimelineSubscriber relay;
        {
            std::lock_guard<std::mutex> lock(host.companion.subscriberMu_);
            relay = host.companion.subscribers_.front();
        }
        const std::string pushed = host.companion.pushCommandFor(relay);
        expect(std::strtol(pushed.c_str(), nullptr, 10) >= web.c,
               "R4: relay subscriber push commandID " + pushed + " < Plex Web c " +
                   std::to_string(web.c));
        std::printf("PLEXWEB_REPLAY row=R4-relay-push push_cmd=%s c=%ld\n", pushed.c_str(), web.c);
    }
    {
        Host host;
        PlexWeb web(host, "plexweb-tab-E");
        web.requestSession();
        web.poll(false);
        playAndFollow(host, web, "R5-owner", true);
        PlexWeb zero(host, "tv-app");
        zero.c = 0;
        zero.poll(true);
        expect(zero.replyCmds.back() == "0",
               "R5: a controller's explicit commandID 0 was replaced by " + zero.replyCmds.back());
        PlexWeb other(host, "phone-app");
        other.c = 3;
        other.poll(true);
        expect(other.replyCmds.back() == "3",
               "R5: a controller's own commandID 3 was replaced by " + other.replyCmds.back());
        host.companion.subscribeTimeline("phone-app", "127.0.0.1", "http", 32500, "3");
        misterplex::Companion::TimelineSubscriber phone;
        {
            std::lock_guard<std::mutex> lock(host.companion.subscriberMu_);
            phone = host.companion.subscribers_.front();
        }
        const std::string pushed = host.companion.pushCommandFor(phone);
        expect(pushed == "3", "R5: subscriber's own commandID 3 pushed as " + pushed);
        std::printf("PLEXWEB_REPLAY row=R5-own-command poll0_cmd=%s poll3_cmd=%s push_cmd=%s\n",
                    zero.replyCmds.back().c_str(), other.replyCmds.back().c_str(), pushed.c_str());
    }
    std::printf("PLEXWEB_REPLAY %s fails=%d\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
