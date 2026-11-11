#pragma once
#include "airplay/ns.hpp"
// GCAST P3: dial_lounge_fe -- YouTube receiver inside misterplexd (GYT=1).
//   SSDP responder (udp 1900, ST urn:dial-multiscreen-org:service:dial:1) + DIAL REST on dialPort
//   (/dd.xml, /apps/YouTube GET/POST/DELETE with additionalData screenId), and the YouTube Lounge
//   BrowserChannel (www.youtube.com/api/lounge/bc/bind) over verified HTTPS (shipped CA bundle).
//   Phones find it via DIAL ("Cast" list) or link with the TV code (pairingCode registration).
// Commands become YtCommand on one worker thread -> main.cpp castPlay(Owner::YouTube, "yt:<id>")
// / pause / resume / stop. Resolution of yt:<id> is done by gcast-xcode on buildhost (yt-dlp): a runtime
// helper dependency, documented in GCAST-DESIGN. Player feedback comes back via report()/ended().
#include <atomic>
#include <chrono>
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
namespace airplay {

struct YtConfig {
    std::string name = "MiSTer";
    std::string hwSeed;                 // -> stable DIAL uuid / Lounge screen uuid
    uint16_t dialPort = 8008;
    std::string screenIdFile;           // persisted Lounge screen_id ("" = new each start)
    std::string caFile;                 // "" = caBundlePath()
    std::string loungeHost = "www.youtube.com";
};

enum class YtCmd { Play, Seek, Pause, Resume, Stop, Volume, Captions };
struct YtCommand {
    YtCmd kind = YtCmd::Stop;
    std::string videoId;
    int64_t ms = 0;
    int64_t fromMs = -1;   // position before a seek; -1 when this is not a seek
    int volume = -1;       // 0-100 for YtCmd::Volume; unused otherwise
    // YtCmd::Captions. captionShow false hides the band. The strings are the
    // lounge track (language, vss id, name, "asr" or empty).
    bool captionShow = false;
    std::string captionLang, captionVss, captionName, captionKind;
};

inline const char* ytCmdName(YtCmd k) {
    switch (k) {
    case YtCmd::Play: return "play";
    case YtCmd::Seek: return "seek";
    case YtCmd::Pause: return "pause";
    case YtCmd::Resume: return "resume";
    case YtCmd::Stop: return "stop";
    case YtCmd::Volume: return "volume";
    case YtCmd::Captions: return "captions";
    }
    return "?";
}

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
    // KI-2: 410 / gracefulReconnect rebind. YouTube stays on Ready and DIAL
    // until this time while the next bind is in flight.
    std::chrono::steady_clock::time_point rebindGraceUntil_{};
    std::atomic<bool> appRunning_{false};
    std::thread ssdpThr_, dialThr_, loungeThr_, sendThr_, workThr_;
    int ssdpFd_ = -1, dialFd_ = -1;
    std::unique_ptr<HttpsClient> pollHttp_, sendHttp_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::string screenId_, loungeToken_;
    bool tokenRecoveryStopped_ = false;  // lounge thread only
    lounge::Channel ch_;
    long rid_ = 1, ofs_ = 0;
    std::deque<lounge::Out> outq_;
    std::deque<YtCommand> cmdq_;
    std::deque<std::string> pairq_;
    // now playing
    std::string videoId_, listId_, cpn_;
    std::vector<std::string> playlist_;
    size_t index_ = 0;
    // Set when a queue edit removed the playing id (or replaced the list
    // without it). Natural end then plays playlist_[0].
    bool queueArmFirst_ = false;
    int state_ = lounge::Unstarted;
    int64_t posMs_ = 0, durMs_ = 0, lastSentPos_ = -100000;
    int lastSentState_ = -99;
    // Last lounge level applied to the player. 100 matches the player default.
    // YT_VOLUME off does not read or write this; the reply stays 100.
    int volumePct_ = 100;
    // Last setSubtitlesTrack while YT_CAPTIONS is on. A later play of another
    // id re-queues this language. Flag off leaves these false and empty.
    bool capShow_ = false;
    std::string capLang_, capVss_, capName_, capKind_;
};

// uuid-shaped id from a seed (FNV-1a), stable per MiSTer.
std::string ytUuidFromSeed(const std::string& seed);

}  // namespace airplay
}  // namespace misterplex
