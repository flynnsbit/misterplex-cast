#pragma once
// GCAST P3: dial_lounge_fe -- YouTube receiver inside misterplexd (GYT=1).
//   SSDP responder (udp 1900, ST urn:dial-multiscreen-org:service:dial:1) + DIAL REST on dialPort
//   (/dd.xml, /apps/YouTube GET/POST/DELETE with additionalData screenId), and the YouTube Lounge
//   BrowserChannel (www.youtube.com/api/lounge/bc/bind) over verified HTTPS (shipped CA bundle).
//   Phones find it via DIAL ("Cast" list) or link with the TV code (pairingCode registration).
// Commands become YtCommand on one worker thread -> main.cpp castPlay(Owner::YouTube, "yt:<id>")
// / pause / resume / stop. Resolution of yt:<id> is done by gcast-xcode on lab-pc (yt-dlp): a runtime
// helper dependency, documented in GCAST-DESIGN. Player feedback comes back via report()/ended().
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "gcast/lounge_proto.hpp"

namespace misterplex {
namespace gcast {

struct YtConfig {
    std::string name = "MiSTer";
    std::string hwSeed;                 // -> stable DIAL uuid / Lounge screen uuid
    uint16_t dialPort = 8008;
    std::string screenIdFile;           // persisted Lounge screen_id ("" = new each start)
    std::string caFile;                 // "" = caBundlePath()
    std::string loungeHost = "www.youtube.com";
};

enum class YtCmd { Play, Seek, Pause, Resume, Stop };
struct YtCommand {
    YtCmd kind = YtCmd::Stop;
    std::string videoId;
    int64_t ms = 0;
    int64_t fromMs = -1;   // position before a seek; -1 when this is not a seek
};

class HttpsClient;

class YtFrontend {
public:
    using Sink = std::function<void(const YtCommand&)>;
    static bool built();
    bool start(const YtConfig& cfg, Sink sink);
    void stop();
    bool running() const { return running_.load(); }
    // True only after the Lounge bind succeeds. Ready must not advertise
    // YouTube, and a play must not start, until this is set.
    bool loungeConnected() const { return loungeUp_.load(); }
    // Player feedback for the current video (state strings as main's setProgress).
    void report(const std::string& state, int64_t posMs, int64_t durMs);
    // The cast ended (natural end, error, or preempted). natural=true may auto-advance a playlist.
    void ended(bool natural);
    std::string screenId() const;
    std::string uuid() const { return uuid_; }
    YtFrontend();
    ~YtFrontend();
    YtFrontend(const YtFrontend&) = delete;
    YtFrontend& operator=(const YtFrontend&) = delete;

private:
    void ssdpLoop();
    void dialLoop();
    void loungeLoop();
    void sendLoop();
    void workerLoop();
    void onLounge(const lounge::Message& m);
    void queueOut(const lounge::Out& o);
    void queueCmd(const YtCommand& c);
    void playIndex(size_t idx, int64_t ms);
    lounge::Out nowPlayingLocked() const;
    bool ensureToken(HttpsClient& c);
    void registerPairing(const std::string& code);

    YtConfig cfg_;
    Sink sink_;
    std::string uuid_;
    std::atomic<bool> running_{false};
    std::atomic<bool> loungeUp_{false};
    std::atomic<bool> appRunning_{false};
    std::thread ssdpThr_, dialThr_, loungeThr_, sendThr_, workThr_;
    int ssdpFd_ = -1, dialFd_ = -1;
    std::unique_ptr<HttpsClient> pollHttp_, sendHttp_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::string screenId_, loungeToken_;
    lounge::Channel ch_;
    long rid_ = 1, ofs_ = 0;
    std::deque<lounge::Out> outq_;
    std::deque<YtCommand> cmdq_;
    std::deque<std::string> pairq_;
    // now playing
    std::string videoId_, listId_, cpn_;
    std::vector<std::string> playlist_;
    size_t index_ = 0;
    int state_ = lounge::Unstarted;
    int64_t posMs_ = 0, durMs_ = 0, lastSentPos_ = -100000;
    int lastSentState_ = -99;
};

// uuid-shaped id from a seed (FNV-1a), stable per MiSTer.
std::string ytUuidFromSeed(const std::string& seed);

}  // namespace gcast
}  // namespace misterplex
