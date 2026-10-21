#pragma once
// GCAST P2: airplay_fe -- AirPlay URL/HLS receiver inside misterplexd on UxPlay's libairplay
// (lib/ only: no GStreamer, internal mdnsd for _airplay._tcp + _raop._tcp, no avahi).
// Built only with ARM_PLEXD_AIRPLAY=1 (MPX_HAVE_AIRPLAY); started only with GAIRPLAY=1.
// libairplay's httpd thread enqueues; one worker thread delivers events to the sink (main.cpp's
// castPlay / seek / pause / stop through the SessionArbiter). GET /playback-info is answered
// from snapshot() without touching the player.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

#include "gcast/airplay_state.hpp"
#include "gcast/raop_audio.hpp"

namespace misterplex {
namespace gcast {

struct AirplayConfig {
    std::string name = "MiSTer";
    uint16_t port = 7000;
    std::string keyfile;      // stable receiver pk ("" = new key each start)
    std::string hwSeed;       // hashed into the locally administered device id
    // RAOP audio (AirPlay music / audio-only senders): decoded PCM is served as a live 48k WAV on
    // audioPort and cast through the normal gcast-xcode path. audioPort 0 = RAOP audio off.
    uint16_t audioPort = 7002;
    std::string audioHost;    // host the helper uses to reach us ("" = local IP toward audioPeerUrl)
    std::string audioPeerUrl; // gcast-xcode base URL (picks the interface for audioHost)
    int audioIdleMs = 3000;   // no RTP audio this long = audio session over (Stop)
};

// "http://host:port/..." -> host ("" if not http)
inline std::string urlHost(const std::string& u) {
    const auto p = u.find("://");
    if (p == std::string::npos)
        return "";
    const auto b = p + 3;
    const auto e = u.find_first_of(":/", b);
    return u.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

// A missing or link-local address is not a place to publish mDNS. 0.0.0.0 is what
// a UDP connect returns when the default route is not up yet.
inline bool airplayAddrUsable(const std::string& ip) {
    if (ip.empty() || ip == "0.0.0.0")
        return false;
    if (ip.rfind("127.", 0) == 0)
        return false;
    if (ip.rfind("169.254.", 0) == 0)
        return false;
    return true;
}

// waits is how many empty lookups have already happened. 1s, 2s, 4s, 8s, then 15s.
inline int airplayAdvertiseDelayMs(int waits) {
    int ms = 1000;
    for (int i = 0; i < waits && ms < 15000; ++i) {
        ms *= 2;
        if (ms > 15000)
            ms = 15000;
    }
    return ms;
}

// Register once a real address exists, again when it changes, and wait with
// backoff while it does not. AirPlay mDNS/RAOP is attempted once at start;
// a boot that wins the race with DHCP otherwise stays unpublished.
struct AirplayAdvertisePlan {
    enum class Action { Wait, Register, Reregister, Hold };
    Action action = Action::Wait;
    int delayMs = 1000;
};

inline AirplayAdvertisePlan planAirplayAdvertise(const std::string& registered, const std::string& now,
                                                 int waits) {
    AirplayAdvertisePlan plan;
    if (!airplayAddrUsable(now)) {
        plan.action = AirplayAdvertisePlan::Action::Wait;
        plan.delayMs = airplayAdvertiseDelayMs(waits);
        return plan;
    }
    if (!airplayAddrUsable(registered)) {
        plan.action = AirplayAdvertisePlan::Action::Register;
        plan.delayMs = 2000;
        return plan;
    }
    if (registered != now) {
        plan.action = AirplayAdvertisePlan::Action::Reregister;
        plan.delayMs = 2000;
        return plan;
    }
    plan.action = AirplayAdvertisePlan::Action::Hold;
    plan.delayMs = 2000;
    return plan;
}

// 02:xx:xx:xx:xx:xx from a seed (FNV-1a), so the device id is stable per MiSTer name.
inline std::string deviceIdFromSeed(const std::string& seed, unsigned char out[6]) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : seed) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    out[0] = 0x02;
    for (int i = 1; i < 6; ++i)
        out[i] = static_cast<unsigned char>(h >> (8 * i));
    char buf[18];
    std::snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", out[0], out[1], out[2], out[3], out[4], out[5]);
    return buf;
}

class AirplayFrontend {
public:
    using Sink = std::function<void(const ApEvent&)>;
    static bool built();
    bool start(const AirplayConfig& cfg, Sink sink);
    void stop();
    bool running() const { return running_.load(); }
    PlaybackSnapshot& snapshot() { return snap_; }
    EventQueue& queue() { return q_; }
    // RAOP audio entry points (called from UxPlay threads via the C callbacks)
    void audioIn(unsigned ct, const uint8_t* data, int len);
    void audioVolume(float db);
    void audioFlushed();
    void audioEnded(const char* why);
    void urlSession(bool on) { urlActive_.store(on); }
    std::string audioUrl() const { return audioUrl_; }
    PcmFanout& pcm() { return pcm_; }
    ~AirplayFrontend() { stop(); }

private:
    void worker();
    void audioServer();
    // true = audio is up, or it cannot come up (no libav, port 0, bind failed).
    // false = no local address yet; the advertise loop retries.
    bool startAudio(const AirplayConfig& cfg);
    void advertiseOnce();
    void audioClient(int fd);
    EventQueue q_;
    PlaybackSnapshot snap_;
    Sink sink_;
    std::atomic<bool> running_{false};
    std::thread thr_;
    void* raop_ = nullptr;
    void* dnssd_ = nullptr;
    PcmFanout pcm_;
    std::atomic<bool> urlActive_{false}, audioActive_{false};
    std::atomic<int64_t> lastAudioMs_{0};
    std::atomic<int> audioClients_{0};
    std::atomic<float> volumeDb_{0.0f};
    std::atomic<bool> decFlush_{false};
    std::string audioUrl_;
    int audioIdleMs_ = 3000;
    int audioFd_ = -1;
    AirplayConfig cfg_{};
    unsigned short httpPort_ = 0;
    std::string advertisedAddr_;
    int advertiseWaits_ = 0;
    int64_t nextAdvertiseMs_ = 0;
    bool audioSettled_ = false;
    std::thread audioThr_;
    void* dec_ = nullptr;   // RaopDecoder (libav builds)
};

}  // namespace gcast
}  // namespace misterplex
