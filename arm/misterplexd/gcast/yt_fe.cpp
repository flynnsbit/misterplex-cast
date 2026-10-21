// GCAST P3 dial_lounge_fe. See yt_fe.hpp.
#include "gcast/lounge_ready.hpp"
#include "gcast/sender_caps.hpp"
#include "gcast/yt_fe.hpp"
#include "wall_clock.hpp"
#include <cstdio>

namespace misterplex {
namespace gcast {

std::string ytUuidFromSeed(const std::string& seed) {
    uint64_t a = 1469598103934665603ULL, b = 0x9E3779B97F4A7C15ULL;
    for (unsigned char c : seed) { a ^= c; a *= 1099511628211ULL; b ^= c; b *= 0x100000001B3ULL; b ^= b >> 29; }
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08x-%04x-4%03x-a%03x-%012llx", unsigned(a >> 32), unsigned(a >> 16) & 0xffff,
                  unsigned(a) & 0xfff, unsigned(b >> 52) & 0xfff, (unsigned long long)(b & 0xffffffffffffULL));
    return buf;
}

}  // namespace gcast
}  // namespace misterplex

#ifndef MPX_HAVE_GCAST_TLS
namespace misterplex {
namespace gcast {
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
}  // namespace gcast
}  // namespace misterplex
#else
#include "gcast/dial_proto.hpp"
#include "gcast/https_client.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <random>
#include <sys/socket.h>
#include <unistd.h>

namespace misterplex {
namespace gcast {
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
// Local IPv4 that routes to `to` (connected UDP socket trick).
std::string localAddrFor(const sockaddr_in& to) {
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
        if (!loungeUp_.load()) {
            char fb[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &from.sin_addr, fb, sizeof fb);
            std::fprintf(stderr, "yt: SSDP ignored; Lounge not connected (%s:%u)\n", fb, ntohs(from.sin_port));
            continue;
        }
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
        int c = ::accept4(dialFd_, nullptr, nullptr, SOCK_CLOEXEC);
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
        if (!ctx.youtubeReady && dial::youtubeAppPath(h.target)) {
            std::fprintf(stderr, "yt: DIAL ignored; Lounge not connected (%s %s)\n", h.method.c_str(),
                         h.target.substr(0, 64).c_str());
            ::close(c);
            continue;
        }
        static const char* reasons[] = {"OK", "Created", "Not Found", "Method Not Allowed", "Payload Too Large"};
        const char* reason = r.status == 200 ? reasons[0] : r.status == 201 ? reasons[1] : r.status == 404 ? reasons[2]
                           : r.status == 405 ? reasons[3] : reasons[4];
        std::string resp = "HTTP/1.1 " + std::to_string(r.status) + " " + reason + "\r\n";
        if (!r.contentType.empty()) resp += "Content-Type: " + r.contentType + "\r\n";
        if (!r.applicationUrl.empty()) resp += "Application-URL: " + r.applicationUrl + "\r\n";
        if (!r.location.empty()) resp += "Location: " + r.location + "\r\n";
        resp += "Access-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(r.body.size()) + "\r\nConnection: close\r\n\r\n" + r.body;
        ::send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
        ::close(c);
        std::fprintf(stderr, "yt: DIAL %s %s -> %d%s%s\n", h.method.c_str(), h.target.substr(0, 64).c_str(), r.status,
                     r.launch ? " launch" : "", r.stop ? " stop" : "");
        if (r.launch) {
            appRunning_.store(true);
            std::lock_guard<std::mutex> lk(mu_);
            if (!r.pairingCode.empty()) caps::pushPairCode(pairq_, r.pairingCode);   // P6: <= 4, no repeats
            if (!r.videoId.empty()) {
                playlist_ = {r.videoId}; index_ = 0; videoId_ = r.videoId; listId_.clear();
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
    std::string sid;
    { std::lock_guard<std::mutex> lk(mu_); sid = screenId_; }
    if (sid.empty() && !cfg_.screenIdFile.empty()) {
        std::ifstream f(cfg_.screenIdFile);
        std::getline(f, sid);
        while (!sid.empty() && (sid.back() == '\n' || sid.back() == '\r' || sid.back() == ' ')) sid.pop_back();
        if (sid.size() < 8 || sid.size() > 64) sid.clear();
    }
    if (sid.empty()) {
        // generate_screen_id has an empty body, no request timestamp, and no token
        // expiry. The only clock dependencies were this client's gate and the
        // certificate notBefore/notAfter check. Skip those two. Chain and hostname stay.
        HttpsRequestFlags sidFlags;
        sidFlags.timeCheck = TlsTimeCheck::Skip;
        HttpsResult r = c.request("GET", cfg_.loungeHost, "/api/lounge/pairing/generate_screen_id", "", "", 15, {},
                                  2u << 20, sidFlags);
        if (!r.ok || r.status != 200 || r.body.size() < 8 || r.body.size() > 64) {
            std::fprintf(stderr, "yt: generate_screen_id failed: status=%d %s\n", r.status, r.err.c_str());
            return false;
        }
        sid = r.body;
        while (!sid.empty() && (sid.back() == '\n' || sid.back() == '\r')) sid.pop_back();
        if (!cfg_.screenIdFile.empty()) { std::ofstream f(cfg_.screenIdFile); f << sid << "\n"; }
        std::fprintf(stderr, "yt: new screen_id %s...\n", sid.substr(0, 6).c_str());
    }
    HttpsResult t = c.request("POST", cfg_.loungeHost, "/api/lounge/pairing/get_lounge_token_batch", "",
                              "screen_ids=" + lounge::formEncode(sid), 15);
    Json j;
    const Json* tok = nullptr;
    if (t.ok && t.status == 200 && parseJson(t.body, j) && j.get("screens") && !j.get("screens")->arr.empty())
        tok = j.get("screens")->arr[0].get("loungeToken");
    if (!tok || tok->type != Json::Str || tok->str.empty()) {
        std::fprintf(stderr, "yt: get_lounge_token_batch failed: status=%d %s\n", t.status, t.err.c_str());
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    screenId_ = sid;
    loungeToken_ = tok->str;
    return true;
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
    auto consume = [this](lounge::ChunkParser& cp, const char* p, size_t n) -> bool {
        std::vector<std::string> payloads;
        bool ok = cp.feed(p, n, payloads);
        for (auto& pl : payloads) {
            std::vector<lounge::Message> ms;
            if (!lounge::decodeBatch(pl, ms)) continue;
            for (auto& m : ms) {
                bool fresh;
                { std::lock_guard<std::mutex> lk(mu_); fresh = ch_.accept(m); }
                if (fresh) onLounge(m);
            }
        }
        if (!ok) std::fprintf(stderr, "yt: bind stream framing error; reconnecting\n");
        return ok && running_.load();
    };
    while (running_.load()) {
        bool haveTok;
        { std::lock_guard<std::mutex> lk(mu_); haveTok = !loungeToken_.empty(); }
        if (!haveTok && !ensureToken(*pollHttp_)) {
            loungeUp_.store(false);
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
                                           [&](const char* p, size_t n) { return consume(cp0, p, n); });
        std::string sid;
        { std::lock_guard<std::mutex> lk(mu_); sid = ch_.sid; }
        if (!r.ok || r.status != 200 || sid.empty()) {
            loungeUp_.store(false);
            std::fprintf(stderr, "yt: bind init failed status=%d %s %s\n", r.status, r.reason.c_str(), r.err.c_str());
            if (r.status == 401 || r.status == 403 || r.status == 410) { std::lock_guard<std::mutex> lk(mu_); loungeToken_.clear(); }
            sleepMs(running_, loungeConnectBackoffMs(failures));
            ++failures;
            continue;
        }
        failures = 0;
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
                                                [&](const char* p, size_t n) { return consume(cp, p, n); },
                                                2u << 20, pollFlags);
            if (!running_.load()) break;
            const lounge::PollNext nx = lounge::nextPoll(pr.ok, pr.status, pr.err, fails + 1);
            if (nx == lounge::PollNext::Stay) { fails = 0; continue; }
            if (nx == lounge::PollNext::RebindKeepReady) {
                // Do not clear loungeUp_: DIAL and the ready line must keep YouTube
                // across this rebind (KI-2). The screen token stays too.
                std::fprintf(stderr, "yt: bind poll idle %ds; re-binding\n", lounge::kLoungePollIdleSec);
                break;
            }
            if (nx == lounge::PollNext::RebindDropReady) {
                std::fprintf(stderr, "yt: bind poll %d %s; re-binding\n", pr.status, pr.reason.c_str());
                if (pr.status == 401 || pr.status == 403 || pr.status == 410) {
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
            static const char* names[] = {"play", "seek", "pause", "resume", "stop"};
            std::fprintf(stderr, "yt: -> %s %s %lld ms\n", names[int(c.kind)], c.videoId.c_str(), (long long)c.ms);
            sink_(c);
        }
    }
}

lounge::Out YtFrontend::nowPlayingLocked() const {
    return lounge::nowPlaying(videoId_, listId_, int(index_), state_, posMs_, durMs_, cpn_);
}

// caller holds mu_
void YtFrontend::playIndex(size_t idx, int64_t ms) {
    if (idx >= playlist_.size()) return;
    index_ = idx; videoId_ = playlist_[idx];
    state_ = lounge::Buffering; posMs_ = ms; durMs_ = 0;
    cpn_ = zx() + zx().substr(0, 4);
    if (cmdq_.size() >= caps::kMaxCmds) cmdq_.pop_front();
    cmdq_.push_back(YtCommand{YtCmd::Play, videoId_, ms});
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
        videoId_.clear(); listId_.clear(); playlist_.clear(); state_ = lounge::Unstarted; posMs_ = durMs_ = 0;
        outq_.push_back(nowPlayingLocked());
        break;
    case lounge::Ev::NowPlaying:
    case lounge::Ev::RemoteJoin:
        outq_.push_back(nowPlayingLocked());
        if (e.kind == lounge::Ev::RemoteJoin) std::fprintf(stderr, "yt: remote connected: %s\n", e.remoteName.c_str());
        break;
    case lounge::Ev::Volume:
        outq_.push_back(lounge::volumeChanged(100));
        break;
    case lounge::Ev::Subtitles:
        outq_.push_back(lounge::subtitlesChanged(videoId_));
        break;
    case lounge::Ev::RemoteLeave:
        std::fprintf(stderr, "yt: remote disconnected: %s\n", e.remoteName.c_str());
        break;
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
    if (natural && index_ + 1 < playlist_.size()) {   // autoplay the next queued video like a TV
        playIndex(index_ + 1, 0);
        return;
    }
    state_ = natural ? lounge::Ended : lounge::Unstarted;
    outq_.push_back(lounge::onStateChange(state_, posMs_, durMs_, cpn_));
    if (!natural) { videoId_.clear(); listId_.clear(); playlist_.clear(); posMs_ = durMs_ = 0; }
    outq_.push_back(nowPlayingLocked());
    lastSentState_ = state_;
    cv_.notify_all();
}

}  // namespace gcast
}  // namespace misterplex
#endif
