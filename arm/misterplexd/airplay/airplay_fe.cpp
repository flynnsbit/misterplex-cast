// GCAST P2: airplay_fe glue onto UxPlay libairplay (see airplay_fe.hpp).
#include "gcast/airplay_fe.hpp"
#include "gcast/airplay_admit.hpp"
#include "gcast/dns_label.hpp"
#include "log_redact.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <vector>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef MPX_HAVE_AIRPLAY
#include "dnssd.h"
#include "gcast/airplay_features.hpp"
#include "logger.h"
#include "raop.h"
#endif

namespace misterplex {
namespace airplay {

#ifndef MPX_HAVE_AIRPLAY
bool AirplayFrontend::built() { return false; }
bool AirplayFrontend::start(const AirplayConfig&, Sink) {
    std::fprintf(stderr, "misterplexd: GAIRPLAY=1 but this build has no AirPlay (ARM_PLEXD_AIRPLAY=0)\n");
    return false;
}
void AirplayFrontend::stop() {}
void AirplayFrontend::worker() {}
void AirplayFrontend::audioIn(unsigned, const uint8_t*, int) {}
void AirplayFrontend::audioVolume(float) {}
void AirplayFrontend::audioFlushed() {}
void AirplayFrontend::audioEnded(const char*) {}
void AirplayFrontend::audioServer() {}
void AirplayFrontend::audioClient(int) {}
bool AirplayFrontend::startAudio(const AirplayConfig&) { return true; }
void AirplayFrontend::advertiseOnce() {}
#else
namespace {
AirplayFrontend* self(void* cls) { return static_cast<AirplayFrontend*>(cls); }
void logCb(void*, int level, const char* msg) {
    if (level <= LOGGER_INFO)
        std::fprintf(stderr, "airplay: %s\n", msg);
}
void audioProcess(void* cls, raop_ntp_t*, audio_decode_struct* a) {
    if (a && a->data)
        self(cls)->audioIn(a->ct, a->data, a->data_len);
}
void videoProcess(void* cls, raop_ntp_t*, video_decode_struct* v) {
    if (v && v->data && v->data_len > 0)
        self(cls)->mirrorIn(v->data, v->data_len, v->ntp_time_remote);
}
void noop(void*) {}
void connReset(void* cls, int reason) {
    std::fprintf(stderr, "airplay: conn_reset reason=%d\n", reason);
    self(cls)->mirrorEnded("conn_reset");
    self(cls)->audioEnded("conn_reset");
}
void videoReset(void* cls, reset_type_t t) {
    std::fprintf(stderr, "airplay: video_reset type=%d\n", static_cast<int>(t));
    self(cls)->mirrorEnded("video_reset");
}
void clientRequest(void*, char* id, char* model, char* name, bool* admit) {
    std::fprintf(stderr, "airplay: client %s model=%s name=%s\n", id ? id : "", model ? model : "",
                 name ? name : "");
    *admit = true;
}
bool checkRegister(void*, const char*) { return true; }
// UxPlay GET /info (with CSeq) calls audio_set_client_volume WITHOUT a null check.
// Missing stub = SEGV in the httpd thread and silent misterplexd death on a real iPhone.
double audioClientVolume(void*) { return 0.0; }
void audioSetVolume(void* cls, float db) { self(cls)->audioVolume(db); }
void audioFlush(void* cls) { self(cls)->audioFlushed(); }
void connDestroy(void* cls) {
    self(cls)->mirrorEnded("conn_destroy");
    self(cls)->audioEnded("conn_destroy");
}
void videoFlush(void* cls) { self(cls)->mirrorEnded("video_flush"); }
void audioGetFormat(void*, unsigned char*, unsigned short*, bool*, bool*, uint64_t*) {}
void videoReportSize(void*, float* w0, float* h0, float* w, float* h) {
    std::fprintf(stderr, "airplay: mirror size source=%.0fx%.0f stream=%.0fx%.0f\n", w0 ? *w0 : 0,
                 h0 ? *h0 : 0, w ? *w : 0, h ? *h : 0);
}
void mirrorVideoRunning(void* cls, bool running) {
    std::fprintf(stderr, "airplay: mirror_video_running=%d\n", running ? 1 : 0);
    if (running)
        self(cls)->mirrorStarted();
    else
        self(cls)->mirrorEnded("mirror_stop");
}
int videoSetCodec(void* cls, video_codec_t c) {
    if (!self(cls)->mirrorEnabled()) {
        std::fprintf(stderr, "airplay: mirror request (codec %d) refused: URL/HLS receiver only\n",
                     static_cast<int>(c));
        return -1;
    }
    if (c == VIDEO_CODEC_H265) {
        std::fprintf(stderr, "airplay: mirror HEVC refused (H.264 only)\n");
        return -1;
    }
    if (!self(cls)->mirrorAcceptCodec()) {
        std::fprintf(stderr, "airplay: SETUP type=110 (mirroring) refused (session busy)\n");
        return -1;
    }
    return 0;
}
int onPlay(void* cls, const char* loc, const float start) {
    const std::string url = loc ? loc : "";
    std::fprintf(stderr, "airplay: /play %.200s start=%.3f\n", misterplex::logUrl(url).c_str(), start);
    // Admit finishes before /play is answered. A refuse is 415 (or 400 if the
    // helper did not answer) and a notice. It does not start playback.
    const AirplayAdmit admit = admitAirplayVideo(url);
    if (!admit.play) {
        std::fprintf(stderr, "airplay: /play refused status=%d notice=%s\n", admit.status, admit.notice.c_str());
        if (!self(cls)->queue().push(noticeEvent(admit)))
            std::fprintf(stderr, "airplay: notice dropped (queue)\n");
        return admit.status == 400 ? 400 : 415;
    }
    self(cls)->urlSession(true);
    self(cls)->snapshot().loading(start);
    if (!self(cls)->queue().push({ApEventType::Play, url, start}))
        std::fprintf(stderr, "airplay: play dropped (queue)\n");
    return 0;
}
void onScrub(void* cls, const float pos) { self(cls)->queue().push({ApEventType::Scrub, "", pos}); }
void onRate(void* cls, const float rate) { self(cls)->queue().push({ApEventType::Rate, "", rate}); }
void onStop(void* cls) {
    self(cls)->urlSession(false);
    self(cls)->queue().push({ApEventType::Stop, "", 0});
}
float onPlaylistRemove(void* cls) {
    PlaybackInfoOut o;
    self(cls)->snapshot().info(o);
    return o.position > 0 ? static_cast<float>(o.position) : 0.f;
}
void onInfo(void* cls, playback_info_t* pi) {
    PlaybackInfoOut o;
    self(cls)->snapshot().info(o);
    pi->duration = o.duration;
    pi->position = o.position;
    pi->seek_start = 0;
    pi->seek_duration = o.duration > 0 ? o.duration : 0;
    pi->rate = static_cast<float>(o.rate);
    pi->ready_to_play = o.ready;
    pi->playback_buffer_empty = !o.ready;
    pi->playback_buffer_full = o.ready;
    pi->playback_likely_to_keep_up = o.ready;
}
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
// Local address the gcast-xcode host routes back to (UDP connect sends nothing).
std::string localIpToward(const std::string& host) {
    if (host.empty())
        return "";
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), "9", &hints, &res) != 0 || !res)
        return "";
    std::string out;
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0 && ::connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
        sockaddr_in me{};
        socklen_t l = sizeof me;
        char buf[INET_ADDRSTRLEN];
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&me), &l) == 0 &&
            inet_ntop(AF_INET, &me.sin_addr, buf, sizeof buf))
            out = buf;
    }
    if (fd >= 0)
        ::close(fd);
    freeaddrinfo(res);
    return out;
}
}  // namespace

void AirplayFrontend::audioIn(unsigned ct, const uint8_t* data, int len) {
#ifdef MPX_HAVE_LIBAV
    if (mirrorActive_.load() && cfg_.onMirrorPcm && dec_) {
        // Screen mirror + audio: the /raop.wav cast path is not used (the mirror owns the screen and the ring).
        auto* mdec = static_cast<RaopDecoder*>(dec_);
        if (decFlush_.exchange(false))
            mdec->flush();
        mdec->setVolumeDb(volumeDb_.load());
        std::vector<uint8_t> mpcm;
        if (mdec->decode(ct, data, len, mpcm) && !mpcm.empty())
            cfg_.onMirrorPcm(mpcm.data(), mpcm.size());
        else if (mdec->errors() == 1 || (mdec->errors() && mdec->errors() % 500 == 0))
            std::fprintf(stderr, "airplay: mirror audio decode error ct=%u len=%d errors=%llu\n", ct, len,
                         static_cast<unsigned long long>(mdec->errors()));
        return;
    }
#endif
    if (urlActive_.load() || audioUrl_.empty())
        return;   // a URL/HLS cast owns the screen; its audio comes through the HLS stream
#ifdef MPX_HAVE_LIBAV
    auto* dec = static_cast<RaopDecoder*>(dec_);
    if (decFlush_.exchange(false))
        dec->flush();
    dec->setVolumeDb(volumeDb_.load());
    std::vector<uint8_t> pcm;
    if (dec->decode(ct, data, len, pcm) && !pcm.empty())
        pcm_.push(pcm.data(), pcm.size());
    else if (dec->errors() == 1 || (dec->errors() && dec->errors() % 500 == 0))
        std::fprintf(stderr, "airplay: audio decode error ct=%u len=%d errors=%llu (every packet failing = wrong AES key: advertised features bit 27 must be off)\n", ct, len,
                     static_cast<unsigned long long>(dec->errors()));
#else
    (void)data;
    (void)len;
#endif
    lastAudioMs_.store(nowMs());
    if (!audioActive_.exchange(true)) {
        std::fprintf(stderr, "airplay: RAOP audio start codec=%s -> %s\n", raopCodecName(raopCodecFromCt(ct)),
                     misterplex::logUrl(audioUrl_).c_str());
        snap_.loading(0);
        if (!q_.push({ApEventType::Play, audioUrl_, 0}))
            std::fprintf(stderr, "airplay: audio play dropped (queue)\n");
    }
}

void AirplayFrontend::audioVolume(float db) {
    volumeDb_.store(db);
    std::fprintf(stderr, "airplay: volume %.1f dB (gain %.3f)\n", db, raopGainFromDb(db));
}

void AirplayFrontend::audioFlushed() {
    decFlush_.store(true);
    pcm_.flush();
}

void AirplayFrontend::audioEnded(const char* why) {
    if (!audioActive_.exchange(false))
        return;
    std::fprintf(stderr, "airplay: RAOP audio end (%s)\n", why);
    decFlush_.store(true);
    pcm_.flush();
    pcm_.closeAll();   // live /raop.wav readers see EOF; the next session attaches fresh listeners
    q_.push({ApEventType::Stop, "", 0});
}

// Minimal live server: GET /raop.wav -> WAV header + PCM while the client stays connected.
void AirplayFrontend::audioServer() {
    while (running_.load()) {
        pollfd p{audioFd_, POLLIN, 0};
        if (::poll(&p, 1, 200) <= 0)
            continue;
        const int c = ::accept(audioFd_, nullptr, nullptr);
        if (c < 0)
            continue;
        if (audioClients_.load() >= 4) {   // helper probe + transcode + slack
            ::close(c);
            continue;
        }
        ++audioClients_;
        std::thread([this, c] { audioClient(c); --audioClients_; }).detach();
    }
}

namespace {
bool sendAll(int fd, const char* b, size_t n) {
    while (n) {
        const ssize_t w = ::send(fd, b, n, MSG_NOSIGNAL);
        if (w <= 0)
            return false;
        b += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}
}  // namespace

void AirplayFrontend::audioClient(int fd) {
    timeval tv{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char req[1024];
    size_t got = 0;
    while (got < sizeof req - 1) {
        const ssize_t n = ::recv(fd, req + got, sizeof req - 1 - got, 0);
        if (n <= 0)
            break;
        got += static_cast<size_t>(n);
        req[got] = 0;
        if (std::strstr(req, "\r\n\r\n"))
            break;
    }
    req[got] = 0;
    const bool head = std::strncmp(req, "HEAD /raop.wav", 14) == 0;
    if (std::strncmp(req, "GET /raop.wav", 13) != 0 && !head) {
        const char nf[] = "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        sendAll(fd, nf, sizeof nf - 1);
        ::close(fd);
        return;
    }
    const std::string hdr = std::string("HTTP/1.0 200 OK\r\nContent-Type: audio/wav\r\nCache-Control: no-store\r\n"
                                        "Connection: close\r\n\r\n") + wavStreamHeader();
    if (!sendAll(fd, hdr.data(), hdr.size()) || head) {
        ::close(fd);
        return;
    }
    auto l = pcm_.attach();
    std::vector<uint8_t> buf(16384);
    uint64_t sent = 0;
    while (running_.load()) {
        const long n = pcm_.pop(l, buf.data(), buf.size(), 200);
        if (n < 0)
            break;
        if (n == 0) {   // sender stall: 50 ms of silence keeps the helper demuxer fed
            if (!audioActive_.load())
                continue;
            std::memset(buf.data(), 0, 9600);
            if (!sendAll(fd, reinterpret_cast<const char*>(buf.data()), 9600))
                break;
            continue;
        }
        if (!sendAll(fd, reinterpret_cast<const char*>(buf.data()), static_cast<size_t>(n)))
            break;
        sent += static_cast<uint64_t>(n);
    }
    pcm_.detach(l);
    std::fprintf(stderr, "airplay: raop.wav client done sent=%llu dropped=%llu\n",
                 static_cast<unsigned long long>(sent), static_cast<unsigned long long>(l->dropped));
    ::close(fd);
}

bool AirplayFrontend::startAudio(const AirplayConfig& cfg) {
    audioIdleMs_ = cfg.audioIdleMs > 0 ? cfg.audioIdleMs : 3000;
#ifndef MPX_HAVE_LIBAV
    std::fprintf(stderr, "airplay: RAOP audio off (no libav in this build)\n");
    return true;
#else
    if (cfg.audioPort == 0)
        return true;
    if (audioFd_ >= 0)
        return true;
    std::string host = cfg.audioHost.empty() ? localIpToward(urlHost(cfg.audioPeerUrl)) : cfg.audioHost;
    // GCAST_XCODE unset (standalone) leaves audioPeerUrl empty, so localIpToward
    // returns "" and the old combined check logged "bind failed" without binding.
    // Learn the default-route address instead. A busy port is a separate log.
    if (host.empty())
        host = localIpToward("8.8.8.8");
    if (!airplayAddrUsable(host)) {
        std::fprintf(stderr,
                     "airplay: RAOP audio off (no local address; GAIRPLAY_AUDIO_HOST and AIRPLAY_XCODE unset; port %u not bound)\n",
                     cfg.audioPort);
        return false;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(cfg.audioPort);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (fd < 0 || ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0 ||
        ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 || ::listen(fd, 4) < 0) {
        std::fprintf(stderr, "airplay: RAOP audio off (host=\"%s\" port %u bind failed errno=%d)\n", host.c_str(),
                     cfg.audioPort, errno);
        if (fd >= 0)
            ::close(fd);
        return true;
    }
    audioFd_ = fd;
    dec_ = new RaopDecoder();
    audioUrl_ = "http://" + host + ":" + std::to_string(cfg.audioPort) + "/raop.wav";
    audioThr_ = std::thread([this] { audioServer(); });
    std::fprintf(stderr, "airplay: RAOP audio on: AAC-ELD/AAC-LC/ALAC -> 48k s16 %s\n", misterplex::logUrl(audioUrl_).c_str());
    return true;
#endif
}

bool AirplayFrontend::built() { return true; }


bool AirplayFrontend::start(const AirplayConfig& cfg, Sink sink) {
    if (running_.load())
        return true;
    // UxPlay httpd uses send() without MSG_NOSIGNAL. A phone that RSTs mid-response
    // would otherwise SIGPIPE-kill the whole daemon (silent: not in crashGuard).
    std::signal(SIGPIPE, SIG_IGN);
    sink_ = std::move(sink);
    unsigned char hw[6];
    const std::string devId = deviceIdFromSeed(cfg.hwSeed.empty() ? cfg.name : cfg.hwSeed, hw);
    int err = 0;
    // One DNS label: the embedded mdnsd splits the instance on '.' ("MiSTerPlex-203.0.113.40" broke avahi).
    const std::string instance = airplayInstanceName(cfg.name);
    if (instance != cfg.name)
        std::fprintf(stderr, "airplay: mDNS instance name \"%s\" (sanitized from \"%s\")\n", instance.c_str(),
                     cfg.name.c_str());
    dnssd_t* d = dnssd_init(instance.c_str(), static_cast<int>(instance.size()), reinterpret_cast<const char*>(hw),
                            6, 0, &err);
    if (err || !d) {
        std::fprintf(stderr, "airplay: dnssd_init failed %d\n", err);
        return false;
    }
    // Default FEATURES_1 still has bit 7 (screen mirroring) on. Clear it so a real
    // iPhone does not try SETUP type=110 unless AIRPLAY_MIRROR is on; bit 27 (legacy
    // pairing) is cleared ALWAYS so the audio/mirror AES key is not ecdh-hashed
    // (fr-airplay-audio-1025: audio-only "audio decode error ct=2"). See airplay_features.hpp.
    {
        ApFeatureEdit fe[5];
        airplayFeatureEdits(cfg.mirror, fe);
        for (const auto& e : fe) {
#ifdef MUT_AP_MIRROR_BIT_ON
            if (e.bit == 7) continue;
#endif
            dnssd_set_airplay_features(d, e.bit, e.val);
        }
    }
    raop_callbacks_t cb;
    std::memset(&cb, 0, sizeof cb);
    cb.cls = this;
    cb.audio_process = audioProcess;
    cb.video_process = videoProcess;
    cb.video_pause = noop;
    cb.video_resume = noop;
    cb.conn_feedback = noop;
    cb.conn_reset = connReset;
    cb.video_reset = videoReset;
    cb.conn_init = noop;
    cb.conn_destroy = connDestroy;
    cb.audio_flush = audioFlush;
    cb.video_flush = videoFlush;
#ifndef MUT_AP_NO_VOLUME_WIRE
    cb.audio_set_client_volume = audioClientVolume;
#endif
    cb.audio_set_volume = audioSetVolume;
    cb.audio_get_format = audioGetFormat;
    cb.video_report_size = videoReportSize;
    cb.mirror_video_running = mirrorVideoRunning;
    cb.report_client_request = clientRequest;
    cb.check_register = checkRegister;
    cb.video_set_codec = videoSetCodec;
    cb.on_video_play = onPlay;
    cb.on_video_scrub = onScrub;
    cb.on_video_rate = onRate;
    cb.on_video_stop = onStop;
    cb.on_video_playlist_remove = onPlaylistRemove;
    cb.on_video_acquire_playback_info = onInfo;
    raop_t* r = raop_init(&cb);
    if (!r) {
        dnssd_destroy(d);
        return false;
    }
    raop_set_log_callback(r, logCb, nullptr);
    raop_set_log_level(r, LOGGER_INFO);
    if (raop_init2(r, 1, devId.c_str(), cfg.keyfile.c_str())) {
        std::fprintf(stderr, "airplay: raop_init2 failed\n");
        raop_destroy(r);
        dnssd_destroy(d);
        return false;
    }
    raop_set_plist(r, "hls", 1);
    cfg_ = cfg;   // onMirrorAu / producer must be live before httpd accepts SETUP 110
    if (cfg.mirror)
        mirrorFeed_.attach(cfg.mirrorProducer);
    // GET /info: one 1280x720 display, 30 fps. refreshRate is stored as Hz;
    // UxPlay writes the plist real as 1/Hz (Apple's frame period).
    raop_set_plist(r, "width", kAirplayInfoWidth);
    raop_set_plist(r, "height", kAirplayInfoHeight);
    raop_set_plist(r, "maxFPS", kAirplayInfoFps);
    raop_set_plist(r, "refreshRate", kAirplayInfoFps);
    dnssd_set_airplay_features(d, 42, 0);   // SupportsScreenMultiCodec / HEVC 4K off
    unsigned short tcp[3] = {7100, cfg.port, 7001}, udp[3] = {7011, 6001, 6000};
    raop_set_tcp_ports(r, tcp);
    raop_set_udp_ports(r, udp);
    unsigned short p = raop_get_port(r);
    if (raop_start_httpd(r, &p) < 0) {
        std::fprintf(stderr, "airplay: httpd on :%u failed\n", cfg.port);
        raop_destroy(r);
        dnssd_destroy(d);
        return false;
    }
    raop_set_port(r, p);
    raop_set_dnssd(r, d);
    raop_ = r;
    dnssd_ = d;
    cfg_ = cfg;
    httpPort_ = p;
    running_.store(true);
    // One shot here, then worker() retries with backoff until a local address
    // exists and re-registers if that address changes. A single register at
    // start is what left the  lab log at "mDNS register failed" / "RAOP audio
    // off (no local address)" when the daemon beat DHCP.
    advertiseOnce();
    thr_ = std::thread([this] { worker(); });
    std::fprintf(stderr, "misterplexd: GAIRPLAY receiver \"%s\" id=%s on :%u features=0x%llx\n", cfg.name.c_str(),
                 devId.c_str(), p, static_cast<unsigned long long>(dnssd_get_airplay_features(d)));
    return true;
}

void AirplayFrontend::stop() {
    if (!running_.exchange(false))
        return;
    if (thr_.joinable())
        thr_.join();
    pcm_.closeAll();
    if (audioThr_.joinable())
        audioThr_.join();
    if (audioFd_ >= 0)
        ::close(audioFd_);
    audioFd_ = -1;
    for (int i = 0; i < 50 && audioClients_.load() > 0; ++i)   // detached clients see running_ false
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto* d = static_cast<dnssd_t*>(dnssd_);
    auto* r = static_cast<raop_t*>(raop_);
    if (d) {
        dnssd_unregister_raop(d);
        dnssd_unregister_airplay(d);
    }
    if (r)
        raop_destroy(r);
    if (d)
        dnssd_destroy(d);
    raop_ = dnssd_ = nullptr;
#ifdef MPX_HAVE_LIBAV
    delete static_cast<RaopDecoder*>(dec_);
#endif
    dec_ = nullptr;
    audioActive_.store(false);
}

namespace {
std::string usableLocalIpv4() {
    const std::string ip = localIpToward("8.8.8.8");
    return airplayAddrUsable(ip) ? ip : std::string();
}

AirplayConfig cfgWithHost(const AirplayConfig& cfg, const std::string& addr) {
    AirplayConfig copy = cfg;
    if (copy.audioHost.empty())
        copy.audioHost = addr;
    return copy;
}
}  // namespace

void AirplayFrontend::advertiseOnce() {
    const int64_t now = nowMs();
    if (now < nextAdvertiseMs_)
        return;
    const std::string addr = usableLocalIpv4();
    const AirplayAdvertisePlan plan = planAirplayAdvertise(advertisedAddr_, addr, advertiseWaits_);
    auto later = [&](int delay) {
        if (delay < 200)
            delay = 200;
        nextAdvertiseMs_ = now + delay;
    };
    if (plan.action == AirplayAdvertisePlan::Action::Wait) {
        if (advertiseWaits_ == 0)
            std::fprintf(stderr, "airplay: mDNS/RAOP waiting for a local address\n");
        ++advertiseWaits_;
        later(plan.delayMs);
        return;
    }
    auto* d = static_cast<dnssd_t*>(dnssd_);
    if (!d) {
        later(plan.delayMs);
        return;
    }
    if (plan.action == AirplayAdvertisePlan::Action::Reregister) {
        std::fprintf(stderr, "airplay: local address %s -> %s, re-register\n", advertisedAddr_.c_str(),
                     addr.c_str());
        dnssd_unregister_raop(d);
        dnssd_unregister_airplay(d);
        advertisedAddr_.clear();
    }
    if (plan.action == AirplayAdvertisePlan::Action::Register ||
        plan.action == AirplayAdvertisePlan::Action::Reregister) {
        const int raopRc = dnssd_register_raop(d, httpPort_);
        const int airRc = raopRc != 0 ? raopRc : dnssd_register_airplay(d, httpPort_);
        if (raopRc != 0 || airRc != 0) {
            std::fprintf(stderr,
                         "airplay: mDNS register failed raop=%d airplay=%d (receiver still reachable by IP; retrying)\n",
                         raopRc, airRc);
            ++advertiseWaits_;
            if (!audioSettled_)
                audioSettled_ = startAudio(cfgWithHost(cfg_, addr));
            later(airplayAdvertiseDelayMs(advertiseWaits_));
            return;
        }
        advertisedAddr_ = addr;
        advertiseWaits_ = 0;
        std::fprintf(stderr, "airplay: mDNS registered on %s\n", addr.c_str());
    }
    if (!audioSettled_)
        audioSettled_ = startAudio(cfgWithHost(cfg_, addr));
    else if (audioFd_ >= 0 && cfg_.audioPort != 0) {
        const std::string want = "http://" + addr + ":" + std::to_string(cfg_.audioPort) + "/raop.wav";
        if (audioUrl_ != want) {
            audioUrl_ = want;
            std::fprintf(stderr, "airplay: RAOP audio address now %s\n", misterplex::logUrl(audioUrl_).c_str());
        }
    }
    later(plan.delayMs);
}

void AirplayFrontend::worker() {
    while (running_.load()) {
        advertiseOnce();
        ApEvent e;
        if (audioActive_.load() && nowMs() - lastAudioMs_.load() > audioIdleMs_)
            audioEnded("idle");
        if (!q_.pop(e, 200))
            continue;
        if (sink_)
            sink_(e);
    }
}
#endif

bool AirplayFrontend::mirrorAcceptCodec() {
    if (!cfg_.mirror)
        return false;
    if (mirror_.state() == MirrorSessionState::Mirroring)
        return false;
    if (mirror_.state() == MirrorSessionState::Listening)
        return true;
    return mirror_.acceptSetup(0);
}

void AirplayFrontend::mirrorStarted() {
    if (!cfg_.mirror)
        return;
    if (mirror_.state() == MirrorSessionState::Idle)
        mirror_.acceptSetup(0);
    if (mirrorActive_.exchange(true))
        return;
    urlActive_.store(true);
    snap_.loading(0);
    mirrorSessionAu_ = false;
    mirrorBadAu_ = 0;
    mirrorFirstAuLogged_ = false;
    mirrorFirstIdrLogged_ = false;
    mirrorAuSecCount_ = 0;
    mirrorAuTotal_ = 0;
    mirrorAuSecStartMs_ = 0;
    const bool attached = static_cast<bool>(cfg_.onMirrorAu) || cfg_.mirrorProducer || mirrorFeed_.producer();
    if (attached)
        std::fprintf(stderr, "airplay: mirror session start (decoder feed attached)\n");
    else
        std::fprintf(stderr, "airplay: mirror session start (decoder feed not attached; FPGA unverified)\n");
    // MediaPlayer owns begin/push/end when onMirrorAu is set (same OwenbRing as Plex).
    if (!cfg_.onMirrorAu && mirrorFeed_.producer()) {
        if (!mirrorFeed_.start())
            std::fprintf(stderr, "airplay: mirror decoder feed start failed (producer busy?)\n");
    }
    if (!q_.push({ApEventType::MirrorStart, "", 0}))
        std::fprintf(stderr, "airplay: mirror start dropped (queue)\n");
}

void AirplayFrontend::mirrorIn(const uint8_t* data, int len, uint64_t ntpNs) {
    if (!cfg_.mirror || !data || len <= 0)
        return;
    if (!mirrorActive_.load())
        mirrorStarted();
    const size_t n = static_cast<size_t>(len);
    // UxPlay flags a failed decrypt / bad NAL framing by setting byte 0 to 1.
    if (!annexBStart(data, n)) {
        if (mirrorBadAu_++ == 0 || (mirrorBadAu_ % 100) == 0)
            std::fprintf(stderr, "airplay: mirror AU dropped (bad framing/decrypt) len=%zu count=%u b0..3=%02x%02x%02x%02x\n",
                         n, mirrorBadAu_, data[0], n > 1 ? data[1] : 0, n > 2 ? data[2] : 0, n > 3 ? data[3] : 0);
        return;
    }
    stashAnnexBSpsPps(data, n, mirrorSps_, mirrorPps_);
    const bool idr = annexBContainsIdr(data, n);
    const bool force = !mirrorSessionAu_ || idr;
    std::vector<uint8_t> prepended;
    const uint8_t* send = data;
    size_t sendN = n;
    if (prependSpsPpsIfNeeded(data, n, mirrorSps_, mirrorPps_, force, prepended)) {
        send = prepended.data();
        sendN = prepended.size();
    }
    (void)ntpNs;   // picture first: arrival-order / kNoPts90k on the FPGA path
    const int64_t pts = h264stream::kNoPts90k;
    if (!mirrorFirstAuLogged_) {
        mirrorFirstAuLogged_ = true;
        std::fprintf(stderr, "airplay: mirror first AU len=%zu idr=%d sps=%zu pps=%zu\n", sendN, idr ? 1 : 0,
                     mirrorSps_.size(), mirrorPps_.size());
    }
    if (idr && !mirrorFirstIdrLogged_) {
        mirrorFirstIdrLogged_ = true;
        std::fprintf(stderr, "airplay: mirror first IDR len=%zu\n", sendN);
    }
    ++mirrorAuTotal_;
    ++mirrorAuSecCount_;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    if (mirrorAuSecStartMs_ == 0)
        mirrorAuSecStartMs_ = now;
    if (now - mirrorAuSecStartMs_ >= 1000) {
        std::fprintf(stderr, "airplay: mirror AUs/s=%u total=%u\n", mirrorAuSecCount_, mirrorAuTotal_);
        mirrorAuSecCount_ = 0;
        mirrorAuSecStartMs_ = now;
    }
    bool ok = false;
    if (cfg_.onMirrorAu)
        ok = cfg_.onMirrorAu(send, sendN, pts);
    else if (mirrorFeed_.active())
        ok = mirrorFeed_.push(send, sendN, pts);
    if (ok)
        mirrorSessionAu_ = true;
    else
        std::fprintf(stderr, "airplay: mirror AU rejected by decoder feed len=%zu\n", sendN);
}

void AirplayFrontend::mirrorEnded(const char* why) {
    if (!mirrorActive_.exchange(false)) {
        if (mirror_.state() != MirrorSessionState::Idle)
            mirror_.resetToReady();
        return;
    }
    std::fprintf(stderr, "airplay: mirror end (%s) aus=%u\n", why ? why : "", mirrorAuTotal_);
    if (!cfg_.onMirrorAu)
        mirrorFeed_.stop();
    mirrorSps_.clear();
    mirrorPps_.clear();
    mirrorSessionAu_ = false;
    mirrorBadAu_ = 0;
    mirror_.resetToReady();
    urlActive_.store(false);
    snap_.ended();
    q_.push({ApEventType::MirrorStop, "", 0});
}

}  // namespace airplay
}  // namespace misterplex
