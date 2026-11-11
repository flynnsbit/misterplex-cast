// GCAST P3 dial_lounge_fe. See yt_fe.hpp.
#include "gcast/lounge_ready.hpp"
#include "gcast/sender_caps.hpp"
#include "gcast/yt_fe.hpp"
#include "wall_clock.hpp"
#include <cstdio>

namespace misterplex {
namespace airplay {

std::string ytUuidFromSeed(const std::string& seed) {
    uint64_t a = 1469598103934665603ULL, b = 0x9E3779B97F4A7C15ULL;
    for (unsigned char c : seed) { a ^= c; a *= 1099511628211ULL; b ^= c; b *= 0x100000001B3ULL; b ^= b >> 29; }
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08x-%04x-4%03x-a%03x-%012llx", unsigned(a >> 32), unsigned(a >> 16) & 0xffff,
                  unsigned(a) & 0xfff, unsigned(b >> 52) & 0xfff, (unsigned long long)(b & 0xffffffffffffULL));
    return buf;
}

}  // namespace airplay
}  // namespace misterplex

#ifndef MPX_HAVE_GCAST_TLS
namespace misterplex {
namespace airplay {
class HttpsClient {};
bool YtFrontend::built() { return false; }
bool YtFrontend::start(const YtConfig&, Sink) {
    std::fprintf(stderr, "misterplexd: GYT=1 but this daemon was built without the gcast TLS deps (ARM_PLEXD_AIRPLAY=1)\n");
    return false;
}
void YtFrontend::stop() {}
void YtFrontend::report(const std::string&, int64_t, int64_t) {}
void YtFrontend::ended(bool) {}
std::string YtFrontend::screenId() const { return std::string(); }
YtFrontend::YtFrontend() {}
YtFrontend::~YtFrontend() {}
}  // namespace airplay
}  // namespace misterplex
#else
#include "gcast/dial_proto.hpp"
#include "libmisterplex/companion_guard.hpp"
#include "gcast/https_client.hpp"
#include "gcast/yt_lounge_token.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <random>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace misterplex {
namespace airplay {
namespace {
std::string zx() {
    static thread_local std::mt19937 rng(std::random_device{}());
    static const char* a = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string s(12, 'a');
    for (auto& c : s) c = a[rng() % 36];
    return s;
}
std::string localAddrOf(int fd) {
    sockaddr_in sa{}; socklen_t l = sizeof sa;
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &l) != 0) return "0.0.0.0";
    char b[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &sa.sin_addr, b, sizeof b);
    return b;
}
// Local IPv4 for SSDP LOCATION. Prefer the RFC1918 iface on the peer's /24
// (GH#8 dual-home). UDP connect() follows the default route, which may not
// be the peer's L2.
std::string localAddrFor(const sockaddr_in& to) {
    char peer[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &to.sin_addr, peer, sizeof peer);
    std::vector<std::string> ips;
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) == 0) {
        for (ifaddrs* i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) ||
                !(i->ifa_flags & IFF_UP))
                continue;
            char b[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, b, sizeof b);
            ips.emplace_back(b);
        }
        freeifaddrs(ifs);
    }
    const std::string same = dial::ssdpLocationIp(peer, ips);
    if (!same.empty())
        return same;
    int s = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return "0.0.0.0";
    std::string r = "0.0.0.0";
    if (::connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof to) == 0) r = localAddrOf(s);
    ::close(s);
    return r;
}
void sleepMs(std::atomic<bool>& run, int ms) {
    for (int i = 0; i < ms / 50 && run.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}
}  // namespace

bool YtFrontend::built() { return true; }

bool YtFrontend::start(const YtConfig& cfg, Sink sink) {
    if (running_.load()) return true;
    std::string clockErr;
    misterplex::WallClockSource clockSrc = misterplex::WallClockSource::None;
    const misterplex::WallClockStatus clock =
        misterplex::waitForWallClockRescue(misterplex::kWallClockRescueWaitMs, &clockErr, &clockSrc);
    if (clock == misterplex::WallClockStatus::Stepped)
        misterplex::logClockSteppedOnce(misterplex::wallClockSourceName(clockSrc));
    else if (clock == misterplex::WallClockStatus::Unavailable && misterplex::offlineClockLogAllowed())
        std::fprintf(stderr, "misterplexd: GYT wall clock implausible: %s\n", clockErr.c_str());
    cfg_ = cfg;
    sink_ = std::move(sink);
    uuid_ = ytUuidFromSeed("yt:" + cfg.hwSeed + ":" + cfg.name);
    const std::string ca = cfg.caFile.empty() ? caBundlePath() : cfg.caFile;
    pollHttp_.reset(new HttpsClient(ca));
    sendHttp_.reset(new HttpsClient(ca));
    if (!pollHttp_->ready()) {
        std::fprintf(stderr, "misterplexd: GYT disabled: %s (ship misterplex-cacert.pem or set MPX_CA_BUNDLE)\n",
                     pollHttp_->initError().c_str());
        return false;
    }
    dialFd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(dialFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons(cfg.dialPort); sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(dialFd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 || ::listen(dialFd_, 8) != 0) {
        std::fprintf(stderr, "misterplexd: GYT disabled: DIAL port %u: %s\n", cfg.dialPort, std::strerror(errno));
        ::close(dialFd_); dialFd_ = -1;
        return false;
    }
    ssdpFd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    setsockopt(ssdpFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in ss{}; ss.sin_family = AF_INET; ss.sin_port = htons(1900); ss.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(ssdpFd_, reinterpret_cast<sockaddr*>(&ss), sizeof ss) != 0) {
        std::fprintf(stderr, "misterplexd: GYT: SSDP bind :1900 failed (%s); TV-code pairing still works\n", std::strerror(errno));
        ::close(ssdpFd_); ssdpFd_ = -1;
    } else {
        ifaddrs* ifs = nullptr; int joined = 0;
        if (getifaddrs(&ifs) == 0) {
            for (ifaddrs* i = ifs; i; i = i->ifa_next) {
                if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
                ip_mreq mr{}; inet_pton(AF_INET, "239.255.255.250", &mr.imr_multiaddr);
                mr.imr_interface = reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr;
                if (setsockopt(ssdpFd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) == 0) ++joined;
            }
            freeifaddrs(ifs);
        }
        std::fprintf(stderr, "misterplexd: GYT SSDP joined 239.255.255.250 on %d interface(s)\n", joined);
    }
    running_.store(true);
    dialThr_ = std::thread(&YtFrontend::dialLoop, this);
    if (ssdpFd_ >= 0) ssdpThr_ = std::thread(&YtFrontend::ssdpLoop, this);
    workThr_ = std::thread(&YtFrontend::workerLoop, this);
    loungeThr_ = std::thread(&YtFrontend::loungeLoop, this);
    sendThr_ = std::thread(&YtFrontend::sendLoop, this);
    std::fprintf(stderr, "misterplexd: GYT receiver \"%s\" uuid=%s DIAL :%u ca=%s\n", cfg.name.c_str(), uuid_.c_str(),
                 cfg.dialPort, ca.c_str());
    return true;
}

void YtFrontend::stop() {
    loungeUp_.store(false);
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (pollHttp_) pollHttp_->cancel();
    if (sendHttp_) sendHttp_->cancel();
    if (dialFd_ >= 0) ::shutdown(dialFd_, SHUT_RDWR);
    if (ssdpFd_ >= 0) ::shutdown(ssdpFd_, SHUT_RDWR);
    for (std::thread* t : {&dialThr_, &ssdpThr_, &workThr_, &loungeThr_, &sendThr_})
        if (t->joinable()) t->join();
    if (dialFd_ >= 0) ::close(dialFd_);
    if (ssdpFd_ >= 0) ::close(ssdpFd_);
    dialFd_ = ssdpFd_ = -1;
}
YtFrontend::YtFrontend() {}
YtFrontend::~YtFrontend() { stop(); }

std::string YtFrontend::screenId() const { std::lock_guard<std::mutex> lk(mu_); return screenId_; }

void YtFrontend::ssdpLoop() {
    char buf[2048];
    while (running_.load()) {
        pollfd p{ssdpFd_, POLLIN, 0};
        if (::poll(&p, 1, 500) <= 0) continue;
        sockaddr_in from{}; socklen_t fl = sizeof from;
        ssize_t n = ::recvfrom(ssdpFd_, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) continue;
        const int mx = dial::dialSearchMx(std::string(buf, size_t(n)));
        if (!mx) continue;
        // GH#8: answer M-SEARCH as soon as DIAL :8008 is up. A phone's first
        // search at boot used to miss us while Lounge was still binding.
        const std::string ip = localAddrFor(from);
        const std::string resp = dial::ssdpResponse(ip, cfg_.dialPort, uuid_, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(mx * 100 < 300 ? mx * 100 : 300));
        ::sendto(ssdpFd_, resp.data(), resp.size(), 0, reinterpret_cast<sockaddr*>(&from), fl);
        char fb[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &from.sin_addr, fb, sizeof fb);
        std::fprintf(stderr, "yt: SSDP M-SEARCH from %s:%u -> LOCATION http://%s:%u/dd.xml\n", fb, ntohs(from.sin_port),
                     ip.c_str(), cfg_.dialPort);
    }
}

void YtFrontend::dialLoop() {
    while (running_.load()) {
        pollfd p{dialFd_, POLLIN, 0};
        if (::poll(&p, 1, 500) <= 0) continue;
        sockaddr_in peerSa{}; socklen_t peerLen = sizeof peerSa;
        int c = ::accept4(dialFd_, reinterpret_cast<sockaddr*>(&peerSa), &peerLen, SOCK_CLOEXEC);
        if (c < 0) continue;
        timeval tv{3, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        std::string raw;
        char b[2048];
        size_t headEnd = std::string::npos;
        caps::Deadline dl(caps::kDialConnMs);   // P6: one sender holds the serial accept loop <= 3 s
        bool timedOut = false;
        auto readable = [&] {
            pollfd q{c, POLLIN, 0};
            if (dl.expired() || ::poll(&q, 1, dl.leftMs()) <= 0) { timedOut = true; return false; }
            return true;
        };
        while (raw.size() <= dial::kMaxHttpHead) {
            if (!readable()) break;
            ssize_t n = ::recv(c, b, sizeof b, 0);
            if (n <= 0) break;
            raw.append(b, size_t(n));
            if ((headEnd = raw.find("\r\n\r\n")) != std::string::npos) break;
        }
        dial::Head h;
        if (headEnd == std::string::npos || !dial::parseHead(raw.substr(0, headEnd + 2), h)) { ::close(c); continue; }
#ifndef MUT_DIAL_NO_ADMIT
        {
            // N2/N3: refuse web pages / DNS rebinding / non-LAN peers before anything is read or acted on.
            static const misterplex::guard::PeerPolicy peerPolicy = [] {
                const char* e = getenv("MPX_COMPANION_ALLOW");
                return misterplex::guard::parsePeerPolicy(e ? e : "");
            }();
            const bool peerLan = peerLen >= sizeof(sockaddr_in) && peerSa.sin_family == AF_INET &&
                                 misterplex::guard::peerAllowed(ntohl(peerSa.sin_addr.s_addr), peerPolicy);
            const dial::Admit ad = dial::admit(h, peerLan);
            if (!ad.ok) {
                static const char k403[] = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                ::send(c, k403, sizeof k403 - 1, MSG_NOSIGNAL);
                ::close(c);
                static std::chrono::steady_clock::time_point lastLog{};
                const auto now = std::chrono::steady_clock::now();
                if (now - lastLog >= std::chrono::seconds(2)) {   // bounded log; no Origin/Host text echoed
                    lastLog = now;
                    std::fprintf(stderr, "yt: DIAL refused %s %s: %s\n", h.method.substr(0, 8).c_str(),
                                 h.target.substr(0, 32).c_str(), ad.why);
                }
                continue;
            }
        }
#endif
        std::string body = raw.substr(headEnd + 4);
        long clen = std::atol(h.get("content-length").c_str());
        if (clen < 0) clen = 0;
        while ((long)body.size() < clen && body.size() <= dial::kMaxHttpBody) {
            if (!readable()) break;
            ssize_t n = ::recv(c, b, sizeof b, 0);
            if (n <= 0) break;
            body.append(b, size_t(n));
        }
        if (timedOut && (long)body.size() < clen) {
            std::fprintf(stderr, "yt: DIAL connection dropped: body not complete in %d ms\n", caps::kDialConnMs);
            ::close(c);
            continue;
        }
        dial::Ctx ctx;
        ctx.name = cfg_.name; ctx.uuid = uuid_; ctx.running = appRunning_.load();
        ctx.screenId = screenId();
        ctx.appUrl = "http://" + localAddrOf(c) + ":" + std::to_string(cfg_.dialPort) + "/apps/";
        ctx.youtubeReady = loungeUp_.load();
        dial::Reply r = dial::handle(h, body, ctx);
        static const char* reasons[] = {"OK", "Created", "Not Found", "Method Not Allowed", "Payload Too Large"};
        const char* reason = r.status == 200 ? reasons[0] : r.status == 201 ? reasons[1] : r.status == 404 ? reasons[2]
                           : r.status == 405 ? reasons[3] : reasons[4];
        std::string resp = "HTTP/1.1 " + std::to_string(r.status) + " " + reason + "\r\n";
        if (!r.contentType.empty()) resp += "Content-Type: " + r.contentType + "\r\n";
        if (!r.applicationUrl.empty()) resp += "Application-URL: " + r.applicationUrl + "\r\n";
        if (!r.location.empty()) resp += "Location: " + r.location + "\r\n";
#ifdef MUT_DIAL_CORS_STAR
        resp += "Access-Control-Allow-Origin: *\r\n";
#endif
        resp += "Content-Length: " + std::to_string(r.body.size()) + "\r\nConnection: close\r\n\r\n" + r.body;
        ::send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
        ::close(c);
        std::fprintf(stderr, "yt: DIAL %s %s -> %d%s%s\n", h.method.c_str(), h.target.substr(0, 64).c_str(), r.status,
                     r.launch ? " launch" : "", r.stop ? " stop" : "");
        if (r.launch) {
            appRunning_.store(true);
            std::lock_guard<std::mutex> lk(mu_);
            if (!r.pairingCode.empty()) caps::pushPairCode(pairq_, r.pairingCode);   // P6: <= 4, no repeats
            if (!r.videoId.empty()) {
                playlist_ = {r.videoId}; index_ = 0; queueArmFirst_ = false; videoId_ = r.videoId; listId_.clear();
                state_ = lounge::Buffering; posMs_ = r.startMs; durMs_ = 0;
                cmdq_.clear();   // P6: latest play wins, as queueCmd
                cmdq_.push_back(YtCommand{YtCmd::Play, r.videoId, r.startMs});
            }
            cv_.notify_all();
        }
        if (r.stop) {
            appRunning_.store(false);
            queueCmd(YtCommand{YtCmd::Stop, "", 0});
        }
    }
}

// screen_id (persisted) + lounge token. Called from the lounge thread only.
bool YtFrontend::ensureToken(HttpsClient& c) {
    std::string sid, token;
    { std::lock_guard<std::mutex> lk(mu_); sid = screenId_; }
    // Only generate_screen_id may skip clock validity; chain and hostname stay verified.
    HttpsRequestFlags sidFlags;
    if (!misterplex::wallTimePlausible(std::time(nullptr)))
        sidFlags.timeCheck = TlsTimeCheck::Skip;
    const YtTokenResult result =
        ytFetchLoungeToken(c, cfg_.loungeHost, cfg_.screenIdFile, uuid_, sid, token, sidFlags);
    tokenRecoveryStopped_ = result == YtTokenResult::Stopped;
    std::lock_guard<std::mutex> lk(mu_);
    screenId_ = sid;
    loungeToken_ = token;
    return result == YtTokenResult::Ready;
}

void YtFrontend::registerPairing(const std::string& code) {
    HttpsClient c(cfg_.caFile.empty() ? caBundlePath() : cfg_.caFile);
    std::string sid = screenId();
    std::string body = "access_type=permanent&app=" + lounge::formEncode("lb-v4") + "&pairing_code=" + lounge::formEncode(code) +
                       "&screen_id=" + lounge::formEncode(sid) + "&screen_name=" + lounge::formEncode(cfg_.name) +
                       "&device_id=" + lounge::formEncode(uuid_);
    HttpsResult r = c.request("POST", cfg_.loungeHost, "/api/lounge/pairing/register_pairing_code", "", body, 15);
    std::fprintf(stderr, "yt: register_pairing_code -> %d %s\n", r.status, r.err.c_str());
}

void YtFrontend::loungeLoop() {
    int failures = 0;
    std::mt19937 rng(std::random_device{}());
    auto ids = [this]() { std::lock_guard<std::mutex> lk(mu_); return lounge::BindIds{uuid_, cfg_.name, loungeToken_}; };
    auto graceOpen = [this]() {
        return rebindGraceUntil_.time_since_epoch().count() != 0 &&
               std::chrono::steady_clock::now() < rebindGraceUntil_;
    };
    auto armGrace = [this]() {
        rebindGraceUntil_ = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(lounge::kLoungeRebindGraceMs);
    };
    auto clearGrace = [this]() { rebindGraceUntil_ = {}; };
    // A failed retry during the rebind grace window must not take YouTube off
    // the Ready line or out of DIAL. 401/403 are not this path: the token is dead.
    auto dropReady = [&]() {
        if (!graceOpen())
            loungeUp_.store(false);
    };
    // endAfterAid: a long-poll that already took a message ends so the next GET
    // carries the new AID (KI-7). The bind-init POST must read the whole reply.
    auto consume = [this](lounge::ChunkParser& cp, const char* p, size_t n, bool endAfterAid) -> bool {
        std::vector<std::string> payloads;
        bool ok = cp.feed(p, n, payloads);
        bool advanced = false;
        for (auto& pl : payloads) {
            std::vector<lounge::Message> ms;
            if (!lounge::decodeBatch(pl, ms)) continue;
            for (auto& m : ms) {
                bool fresh = false;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    const long before = ch_.aid;
                    fresh = ch_.accept(m);
                    if (ch_.aid != before)
                        advanced = true;
                }
                if (fresh) onLounge(m);
            }
        }
        if (!ok) std::fprintf(stderr, "yt: bind stream framing error; reconnecting\n");
        if (!ok)
            return false;
        if (lounge::endPollForNewAid(advanced, endAfterAid))
            return false;
        return running_.load();
    };
    while (running_.load()) {
        bool haveTok;
        { std::lock_guard<std::mutex> lk(mu_); haveTok = !loungeToken_.empty(); }
        if (!haveTok && !ensureToken(*pollHttp_)) {
            if (tokenRecoveryStopped_) {
                loungeUp_.store(false);
                std::fprintf(stderr, "yt: token recovery stopped; resolve the error and restart GYT to retry\n");
                break;
            }
            dropReady();
            // Screen-id itself does not need a sane clock. Later lounge calls do.
            // This is the current time(), not "the rescue has stepped". A clock
            // that is already 2026 must be allowed on the first pass.
            if (!misterplex::wallTimePlausible(std::time(nullptr))) {
                std::string clockErr;
                misterplex::WallClockSource src = misterplex::WallClockSource::None;
                const auto st = misterplex::waitForWallClockRescue(misterplex::kWallClockRescueWaitMs, &clockErr, &src);
                if (st == misterplex::WallClockStatus::Stepped)
                    std::fprintf(stderr, "misterplexd: GYT stepped the wall clock via %s after screen-id failure\n",
                                 misterplex::wallClockSourceName(src));
                else if (st == misterplex::WallClockStatus::Unavailable)
                    std::fprintf(stderr, "misterplexd: GYT wall clock implausible: %s\n", clockErr.c_str());
            }
            sleepMs(running_, loungeConnectBackoffMs(failures));
            ++failures;
            continue;
        }
        long rid;
        { std::lock_guard<std::mutex> lk(mu_); ch_.reset(); ofs_ = 0; rid_ = 10000 + long(rng() % 80000); rid = rid_++; }
        lounge::ChunkParser cp0;
        HttpsResult r = pollHttp_->request("POST", cfg_.loungeHost, lounge::bindInitialPath(ids(), rid, zx()), "", "count=0", 20,
                                           [&](const char* p, size_t n) { return consume(cp0, p, n, false); });
        std::string sid;
        { std::lock_guard<std::mutex> lk(mu_); sid = ch_.sid; }
        if (!r.ok || r.status != 200 || sid.empty()) {
            std::fprintf(stderr, "yt: bind init failed status=%d %s %s\n", r.status, r.reason.c_str(), r.err.c_str());
            if (r.status == 401 || r.status == 403) {
                clearGrace();
                loungeUp_.store(false);
                std::lock_guard<std::mutex> lk(mu_);
                loungeToken_.clear();
            } else if (r.status == 410) {
                armGrace();
                std::lock_guard<std::mutex> lk(mu_);
                loungeToken_.clear();
            } else {
                dropReady();
            }
            sleepMs(running_, loungeConnectBackoffMs(failures));
            ++failures;
            continue;
        }
        failures = 0;
        clearGrace();
        loungeUp_.store(true);
        std::fprintf(stderr, "yt: Lounge connected (screen %s...); waiting for a phone\n", screenId().substr(0, 6).c_str());
        { std::lock_guard<std::mutex> lk(mu_); outq_.push_back(nowPlayingLocked()); cv_.notify_all(); }
        int fails = 0;
        while (running_.load()) {
            lounge::Channel snap;
            { std::lock_guard<std::mutex> lk(mu_); snap = ch_; }
            lounge::ChunkParser cp;
            HttpsRequestFlags pollFlags;
            pollFlags.idleSec = lounge::kLoungePollIdleSec;
            HttpsResult pr = pollHttp_->request("GET", cfg_.loungeHost, lounge::bindPollPath(ids(), snap, zx()), "", "", 300,
                                                [&](const char* p, size_t n) { return consume(cp, p, n, true); },
                                                2u << 20, pollFlags);
            if (!running_.load()) break;
            const lounge::PollNext nx = lounge::nextPoll(pr.ok, pr.status, pr.err, fails + 1);
            if (nx == lounge::PollNext::Stay) { fails = 0; continue; }
            if (nx == lounge::PollNext::PollAgain) {
                // Same SID, new GET, updated AID. Quiet for one idle window is
                // not a reason to drop YouTube or mint a new screen session.
                ++fails;
                continue;
            }
            if (nx == lounge::PollNext::RebindKeepReady) {
                // Do not clear loungeUp_: DIAL and the ready line keep YouTube
                // across this rebind (KI-2). 410 retires the lounge token; an
                // idle rebind keeps it. The grace window covers the next bind.
                armGrace();
                if (pr.status == 410) {
                    std::fprintf(stderr, "yt: bind poll %d %s; re-binding\n", pr.status, pr.reason.c_str());
                    std::lock_guard<std::mutex> lk(mu_);
                    loungeToken_.clear();
                } else {
                    std::fprintf(stderr, "yt: bind poll idle %ds; re-binding\n", lounge::kLoungePollIdleSec);
                }
                break;
            }
            if (nx == lounge::PollNext::RebindDropReady) {
                std::fprintf(stderr, "yt: bind poll %d %s; re-binding\n", pr.status, pr.reason.c_str());
                clearGrace();
                if (pr.status == 401 || pr.status == 403) {
                    std::lock_guard<std::mutex> lk(mu_);
                    loungeToken_.clear();
                }
                loungeUp_.store(false);
                break;
            }
            ++fails;
            sleepMs(running_, 1000 * fails);
        }
    }
}

void YtFrontend::queueOut(const lounge::Out& o) {
    std::lock_guard<std::mutex> lk(mu_);
#ifndef MUT_YT_NO_OUTCAP
    if (outq_.size() >= 64) outq_.pop_front();
#endif
    outq_.push_back(o);
    cv_.notify_all();
}
void YtFrontend::queueCmd(const YtCommand& c) {
    std::lock_guard<std::mutex> lk(mu_);
    if (c.kind == YtCmd::Play || c.kind == YtCmd::Stop) cmdq_.clear();   // latest play/stop wins
    if (cmdq_.size() >= caps::kMaxCmds) cmdq_.pop_front();
    cmdq_.push_back(c);
    cv_.notify_all();
}

void YtFrontend::sendLoop() {
    while (running_.load()) {
        std::vector<lounge::Out> batch;
        lounge::BindIds ids; lounge::Channel ch; long rid = 0, ofs = 0;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(500), [&] { return !running_.load() || (!outq_.empty() && !ch_.sid.empty()); });
            if (!running_.load()) break;
            if (outq_.empty() || ch_.sid.empty()) continue;
            while (!outq_.empty() && batch.size() < 10) { batch.push_back(outq_.front()); outq_.pop_front(); }
            ids = lounge::BindIds{uuid_, cfg_.name, loungeToken_}; ch = ch_; rid = rid_++; ofs = ofs_;
        }
        const std::string body = lounge::formBody(batch, ofs);
        HttpsResult r;
        for (int attempt = 0; attempt < 2 && running_.load(); ++attempt) {
            r = sendHttp_->request("POST", cfg_.loungeHost, lounge::bindSendPath(ids, ch, rid, zx()), "", body, 15);
            if (r.ok && r.status == 200) break;
            sleepMs(running_, 500);
        }
        if (r.ok && r.status == 200) {
            std::lock_guard<std::mutex> lk(mu_);
            if (ch_.sid == ch.sid) ofs_ = ofs + long(batch.size());
            std::string names;
            for (auto& o : batch) names += (names.empty() ? "" : ",") + o.cmd;
            std::fprintf(stderr, "yt: sent %s\n", names.c_str());
        } else {
            std::fprintf(stderr, "yt: send failed status=%d %s (dropped %zu)\n", r.status, r.err.c_str(), batch.size());
        }
    }
}

void YtFrontend::workerLoop() {
    while (running_.load()) {
        YtCommand c; std::string pair; bool haveCmd = false;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(500), [&] {
                return !running_.load() || !cmdq_.empty() || (!pairq_.empty() && !screenId_.empty());
            });
            if (!running_.load()) break;
            if (!pairq_.empty() && !screenId_.empty()) { pair = pairq_.front(); pairq_.pop_front(); }
            if (!cmdq_.empty()) { c = cmdq_.front(); cmdq_.pop_front(); haveCmd = true; }
        }
        if (!pair.empty()) registerPairing(pair);
        if (haveCmd && sink_) {
            if (c.kind == YtCmd::Volume)
                std::fprintf(stderr, "yt: -> volume %d\n", c.volume);
            else
                std::fprintf(stderr, "yt: -> %s %s %lld ms\n", ytCmdName(c.kind), c.videoId.c_str(), (long long)c.ms);
            sink_(c);
        }
    }
}

lounge::Out YtFrontend::nowPlayingLocked() const {
    return lounge::nowPlaying(videoId_, listId_, int(index_), state_, posMs_, durMs_, cpn_);
}

// caller holds mu_
void YtFrontend::playIndex(size_t idx, int64_t ms) {
    queueArmFirst_ = false;
    if (idx >= playlist_.size()) return;
    index_ = idx; videoId_ = playlist_[idx];
    state_ = lounge::Buffering; posMs_ = ms; durMs_ = 0;
    cpn_ = zx() + zx().substr(0, 4);
    if (cmdq_.size() >= caps::kMaxCmds) cmdq_.pop_front();
    cmdq_.push_back(YtCommand{YtCmd::Play, videoId_, ms});
    // CC that was on follows this id. The play command drops the previous
    // cues; this fetch arms the new one. Flag off never sets capShow_.
    if (lounge::ytCaptionsEnabled() && capShow_ && !capLang_.empty()) {
        YtCommand cap;
        cap.kind = YtCmd::Captions;
        cap.captionShow = true;
        cap.videoId = videoId_;
        cap.captionLang = capLang_;
        cap.captionVss = capVss_;
        cap.captionName = capName_;
        cap.captionKind = capKind_;
        cmdq_.push_back(cap);
    }
    outq_.push_back(nowPlayingLocked());
    outq_.push_back(lounge::onStateChange(state_, posMs_, durMs_, cpn_));
    lastSentState_ = state_;
    cv_.notify_all();
}

void YtFrontend::onLounge(const lounge::Message& m) {
    const lounge::Event e = lounge::toEvent(m);
    if (m.cmd != "loungeStatus") std::fprintf(stderr, "yt: lounge #%ld %s\n", m.index, m.cmd.c_str());
    std::lock_guard<std::mutex> lk(mu_);
    switch (e.kind) {
    case lounge::Ev::Play:
        playlist_ = e.videoIds.empty() ? std::vector<std::string>{e.videoId} : e.videoIds;
        listId_ = e.listId;
        {
            size_t idx = size_t(e.index);
            if (idx >= playlist_.size() || playlist_[idx] != e.videoId) {
                idx = 0;
                for (size_t i = 0; i < playlist_.size(); ++i) if (playlist_[i] == e.videoId) { idx = i; break; }
                if (playlist_.empty() || playlist_[idx] != e.videoId) { playlist_ = {e.videoId}; idx = 0; }
            }
            cmdq_.clear();
            playIndex(idx, e.ms);
        }
        break;
    case lounge::Ev::Seek:
        if (videoId_.empty()) break;
        {
            const int64_t from = posMs_;
            state_ = lounge::Buffering; posMs_ = e.ms;
            cmdq_.push_back(YtCommand{YtCmd::Seek, videoId_, e.ms, from});
        }
        outq_.push_back(lounge::onStateChange(state_, posMs_, durMs_, cpn_));
        lastSentState_ = state_;
        break;
    case lounge::Ev::Pause:
        if (videoId_.empty()) break;
        state_ = lounge::Paused;
        cmdq_.push_back(YtCommand{YtCmd::Pause, videoId_, posMs_});
        outq_.push_back(lounge::onStateChange(state_, posMs_, durMs_, cpn_));
        lastSentState_ = state_;
        break;
    case lounge::Ev::Resume:
        if (videoId_.empty()) break;
        state_ = lounge::Playing;
        cmdq_.push_back(YtCommand{YtCmd::Resume, videoId_, posMs_});
        outq_.push_back(lounge::onStateChange(state_, posMs_, durMs_, cpn_));
        lastSentState_ = state_;
        break;
    case lounge::Ev::Stop:
        cmdq_.clear();
        cmdq_.push_back(YtCommand{YtCmd::Stop, videoId_, 0});
        capShow_ = false;
        capLang_.clear();
        capVss_.clear();
        capName_.clear();
        capKind_.clear();
        videoId_.clear(); listId_.clear(); playlist_.clear(); queueArmFirst_ = false;
        state_ = lounge::Unstarted; posMs_ = durMs_ = 0;
        outq_.push_back(nowPlayingLocked());
        break;
    case lounge::Ev::NowPlaying:
    case lounge::Ev::RemoteJoin:
        outq_.push_back(nowPlayingLocked());
        if (e.kind == lounge::Ev::RemoteJoin) std::fprintf(stderr, "yt: remote connected: %s\n", e.remoteName.c_str());
        break;
    case lounge::Ev::Volume: {
        // YT_VOLUME off: loungeVolumeReply is 100 and nothing is queued (tip path).
        // On: a parsed 0-100 setVolume is YtCmd::Volume for player setVolumePercent.
        // getVolume replies the last applied level and does not change it.
        const bool on = lounge::ytVolumeEnabled();
        if (lounge::loungeVolumeApplies(on, e)) {
            volumePct_ = e.volume;
            YtCommand c;
            c.kind = YtCmd::Volume;
            c.volume = volumePct_;
            cmdq_.push_back(c);
        }
        outq_.push_back(lounge::volumeChanged(lounge::loungeVolumeReply(on, e, volumePct_)));
        break;
    }
    case lounge::Ev::Subtitles: {
        // YT_CAPTIONS off: getSubtitlesTrack still acks with the video id only.
        // setSubtitlesTrack never reaches here (it stays Ev::None).
        // On: a set show/hides the track, and the ack names the language.
        const bool on = lounge::ytCaptionsEnabled();
        if (lounge::loungeCaptionApplies(on, e)) {
            YtCommand c;
            c.kind = YtCmd::Captions;
            c.videoId = !e.videoId.empty() ? e.videoId : videoId_;
            c.captionShow = e.captionShow && !e.captionLang.empty();
            if (c.captionShow) {
                capShow_ = true;
                capLang_ = e.captionLang;
                capVss_ = e.captionVss;
                capName_ = e.captionName;
                capKind_ = e.captionKind;
                c.captionLang = capLang_;
                c.captionVss = capVss_;
                c.captionName = capName_;
                c.captionKind = capKind_;
            } else {
                capShow_ = false;
                capLang_.clear();
                capVss_.clear();
                capName_.clear();
                capKind_.clear();
            }
            cmdq_.push_back(c);
        }
        lounge::CaptionEcho echo;
        echo.show = capShow_;
        echo.lang = capLang_;
        echo.name = capName_;
        echo.vss = capVss_;
        echo.kind = capKind_;
        outq_.push_back(lounge::captionLoungeReply(on, videoId_, echo));
        break;
    }
    case lounge::Ev::RemoteLeave:
        std::fprintf(stderr, "yt: remote disconnected: %s\n", e.remoteName.c_str());
        break;
    case lounge::Ev::Next:
    case lounge::Ev::Prev: {
        // YT_NEXT_PREV on: skip to the neighbor in this queue. A miss stays put.
        // Flag off never reaches here. Natural end stays in ended().
        if (videoId_.empty()) break;
        lounge::IdList q;
        q.ids = playlist_;
        q.index = index_;
        q.endPlaysFirst = queueArmFirst_;
        size_t at = 0;
        const lounge::SkipDir dir = e.kind == lounge::Ev::Next ? lounge::SkipDir::Next : lounge::SkipDir::Prev;
        if (!lounge::queueSkip(q, dir, at)) break;
        cmdq_.clear();
        playIndex(at, 0);
        break;
    }
    case lounge::Ev::Queue: {
        // Queue edit after the first play. The current video keeps running.
        lounge::IdList q;
        q.ids = playlist_;
        q.index = index_;
        q.endPlaysFirst = queueArmFirst_;
        if (!lounge::applyLoungeQueue(q, e, videoId_)) break;
        playlist_ = std::move(q.ids);
        queueArmFirst_ = q.endPlaysFirst;
        if (!queueArmFirst_) index_ = q.index;
        if (!e.listId.empty()) listId_ = e.listId;
        if (!queueArmFirst_) outq_.push_back(nowPlayingLocked());
        break;
    }
    case lounge::Ev::None:
        break;
    }
    while (outq_.size() > 64) outq_.pop_front();
    cv_.notify_all();
}

void YtFrontend::report(const std::string& st, int64_t posMs, int64_t durMs) {
    std::lock_guard<std::mutex> lk(mu_);
    if (videoId_.empty()) return;
    int s = state_;
    if (st == "playing") s = lounge::Playing;
    else if (st == "paused") s = lounge::Paused;
    else if (st == "buffering") s = lounge::Buffering;
    if (posMs >= 0) posMs_ = posMs;
    if (durMs > 0) durMs_ = durMs;
    const bool changed = s != lastSentState_;
    state_ = s;
    if (changed || (s == lounge::Playing && std::llabs(posMs_ - lastSentPos_) >= 10000)) {
        outq_.push_back(lounge::onStateChange(state_, posMs_, durMs_, cpn_));
        while (outq_.size() > 64) outq_.pop_front();
        lastSentState_ = s; lastSentPos_ = posMs_;
        cv_.notify_all();
    }
}

void YtFrontend::ended(bool natural) {
    std::lock_guard<std::mutex> lk(mu_);
    if (videoId_.empty()) return;
    lounge::IdList q;
    q.ids = playlist_;
    q.index = index_;
    q.endPlaysFirst = queueArmFirst_;
    size_t next = 0;
    if (natural && lounge::naturalEndNext(q, next)) {   // autoplay the next queued video like a TV
        playIndex(next, 0);
        return;
    }
    state_ = natural ? lounge::Ended : lounge::Unstarted;
    outq_.push_back(lounge::onStateChange(state_, posMs_, durMs_, cpn_));
    if (!natural) { videoId_.clear(); listId_.clear(); playlist_.clear(); queueArmFirst_ = false; posMs_ = durMs_ = 0; }
    outq_.push_back(nowPlayingLocked());
    lastSentState_ = state_;
    cv_.notify_all();
}

}  // namespace airplay
}  // namespace misterplex
#endif
