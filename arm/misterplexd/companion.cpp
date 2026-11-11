#include "companion.hpp"
#include "companion_cors.hpp"
#include "companion_poll_hold.hpp"
#include "plex_tv_link.hpp"

#include "libmisterplex/plexui/plexui_http.hpp"
#include "log_redact.hpp"
#include "companion_reqlog.hpp"
#include "plex_keys.hpp"
#include "plex_resolve.hpp"
#include "pms_reach.hpp"

#include "libmisterplex/play_queue_version.hpp"

#include "libmisterplex/gdm_filter.hpp"
#include "libmisterplex/companion_guard.hpp"
#include "libmisterplex/lan_auth.hpp"
#include "libmisterplex/h264_nal.hpp"
#include "libmisterplex/mast_status_http.hpp"
#include "libmisterplex/phone_status_http.hpp"
#include "libmisterplex/report_code.hpp"
#include "libmisterplex/zaparoo.hpp"
#include "libmisterplex/subtitle_cues.hpp"
#include "libmisterplex/sps_crop_letterbox.hpp"
#include "libmisterplex/core_gate.hpp"
#include "libmisterplex/resume_zone.hpp"

#ifdef MPX_480P_BITRATE_FLOOR
// 480p min bitrate floor (kbps): default 1000 kbps so PMS holds Plex-quality rate
#define MPX_480P_BITRATE_FLOOR_KBPS 1000
#endif

#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <algorithm>
#include <deque>
#include <sstream>
#include <pthread.h>
#include <thread>
#include <vector>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

namespace misterplex {

bool spsRejectHigh(const std::vector<uint8_t>& nalu) {
    size_t i = 0;
    if (nalu.size() >= 4 && nalu[0] == 0 && nalu[1] == 0 && nalu[2] == 0 && nalu[3] == 1)
        i = 4;
    else if (nalu.size() >= 3 && nalu[0] == 0 && nalu[1] == 0 && nalu[2] == 1)
        i = 3;
    if (nalu.size() < i + 2)
        return false;
    const uint8_t hdr = nalu[i];
    if ((hdr & 0x80) != 0)
        return true;
    const uint8_t type = static_cast<uint8_t>(hdr & 0x1f);
    if (type == 7) {
        // SPS: Baseline profile_idc is 66. High (100) and Main (77) reject.
        return nalu[i + 1] != 66;
    }
    if (type == 8) {
        // PPS: CABAC is entropy_coding_mode_flag, not an SPS field.
        const auto pps = parsePpsRbsp(nalu.data() + i + 1, nalu.size() - i - 1);
        return pps.entropy_cabac;
    }
    return false;
}

namespace {

size_t findAnnexStart(const uint8_t* p, size_t n, size_t from) {
    for (size_t i = from; i + 2 < n; ++i) {
        if (p[i] != 0 || p[i + 1] != 0)
            continue;
        if (p[i + 2] == 1)
            return i;
        if (i + 3 < n && p[i + 2] == 0 && p[i + 3] == 1)
            return i;
    }
    return n;
}

size_t startCodeLenAt(const uint8_t* p, size_t n, size_t i) {
    if (i + 3 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1)
        return 4;
    if (i + 2 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)
        return 3;
    return 0;
}

bool nalIsHighOrCabac(const uint8_t* p, size_t n) {
    if (!p || n < 2)
        return false;
    if (!spsRejectHigh(std::vector<uint8_t>(p, p + n)))
        return false;
    size_t i = 0;
    const size_t sc = startCodeLenAt(p, n, 0);
    if (sc)
        i = sc;
    if (n < i + 1)
        return false;
    const uint8_t type = static_cast<uint8_t>(p[i] & 0x1f);
    if (type == 8)
        return true;
    return type == 7 && n > i + 1 && p[i + 1] == 100;
}

}  // namespace

bool annexbNeedsBaselineTranscode(const uint8_t* p, size_t n) {
    if (!p || n < 2)
        return false;
    const size_t first = findAnnexStart(p, n, 0);
    if (first >= n)
        return nalIsHighOrCabac(p, n);
    size_t sc = first;
    while (sc < n) {
        const size_t sl = startCodeLenAt(p, n, sc);
        if (!sl)
            break;
        const size_t next = findAnnexStart(p, n, sc + sl);
        if (next <= sc)
            break;
        if (nalIsHighOrCabac(p + sc, next - sc))
            return true;
        sc = next;
    }
    return false;
}

bool highLibavTranscodeEnabled() {
    const char* e = std::getenv("MPX_HIGH_LIBAV_TRANSCODE");
    if (!e || !e[0])
        return true;
    return std::strcmp(e, "0") != 0 && std::strcmp(e, "off") != 0 && std::strcmp(e, "false") != 0;
}

bool baselineHostTranscodeEnabled() {
    const char* e = std::getenv("MPX_BASELINE_HOST_TRANSCODE");
    if (e && e[0] == '0' && e[1] == '\0')
        return false;
    return true;
}

std::vector<std::string> baselineHostFfmpegArgs(const std::string& ffmpegPath) {
    // Inverse of the High rewrite: PMS still returns profile_idc 100 after
    // cabac=0. Baseline CAVLC, one ref, no B, so a baseline core can take it.
    return {
        ffmpegPath, "-hide_banner", "-loglevel", "error", "-nostdin",
        "-probesize", "4096",
        "-r", "24", "-f", "h264", "-i", "pipe:0",
        "-an", "-threads", "2",
        "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
        "-profile:v", "baseline", "-level", "3.0", "-pix_fmt", "yuv420p",
        "-refs", "1", "-bf", "0", "-coder", "cavlc",
        "-g", "24", "-keyint_min", "24",
        "-x264-params",
        "cabac=0:bframes=0:ref=1:weightp=0:8x8dct=0:partitions=none:"
        "scenecut=0:open-gop=0:intra-refresh=0:aud=1",
        "-flush_packets", "1",
        "-f", "h264", "pipe:1",
    };
}

bool highHostCabacTranscodeEnabled() {
    const char* e = std::getenv("MPX_HIGH_HOST_CABAC");
    if (!e || !e[0])
        return true;
    return std::strcmp(e, "0") != 0 && std::strcmp(e, "off") != 0 && std::strcmp(e, "false") != 0;
}

std::vector<std::string> highCabacFfmpegArgs(const std::string& ffmpegPath) {
    // Box ffmpeg has libx264. This process is ours, so the encoder pin stays.
    // Main + CABAC, one ref, no B, no 8x8: the High core's PPS contract.
    return {
        ffmpegPath, "-hide_banner", "-loglevel", "error", "-nostdin",
        "-probesize", "4096",
        "-r", "24", "-f", "h264", "-i", "pipe:0",
        "-an", "-threads", "2",
        "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
        "-profile:v", "main", "-level", "3.0", "-pix_fmt", "yuv420p",
        "-refs", "1", "-bf", "0", "-coder", "cabac",
        "-g", "24", "-keyint_min", "24",
        "-x264-params",
        "cabac=1:bframes=0:ref=1:weightp=0:8x8dct=0:partitions=none:"
        "scenecut=0:open-gop=0:intra-refresh=0:aud=1",
        "-flush_packets", "1",
        "-f", "h264", "pipe:1",
    };
}

namespace {

size_t annexbStartLen(const uint8_t* p, size_t n, size_t i) {
    if (i + 3 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1)
        return 4;
    if (i + 2 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)
        return 3;
    return 0;
}

int annexbNalType(const uint8_t* p, size_t n, size_t i) {
    const size_t sl = annexbStartLen(p, n, i);
    if (!sl || i + sl >= n)
        return -1;
    return p[i + sl] & 0x1f;
}

}  // namespace

bool annexbContainsIdr(const uint8_t* p, size_t n) {
    if (!p || n < 4)
        return false;
    for (size_t i = 0; i + 3 < n;) {
        const size_t sl = annexbStartLen(p, n, i);
        if (!sl) {
            ++i;
            continue;
        }
        if (annexbNalType(p, n, i) == 5)
            return true;
        i += sl;
    }
    return false;
}

size_t extractCompleteAnnexbAus(std::vector<uint8_t>& buf,
                                std::vector<std::vector<uint8_t>>& out) {
    size_t emitted = 0;
    while (buf.size() >= 4) {
        std::vector<size_t> scs;
        for (size_t i = 0; i + 3 < buf.size();) {
            const size_t sl = annexbStartLen(buf.data(), buf.size(), i);
            if (!sl) {
                ++i;
                continue;
            }
            scs.push_back(i);
            i += sl;
        }
        int vcl = -1;
        for (size_t k = 0; k < scs.size(); ++k) {
            const int t = annexbNalType(buf.data(), buf.size(), scs[k]);
            if ((t == 1 || t == 5) && k + 1 < scs.size()) {
                vcl = static_cast<int>(k);
                break;
            }
        }
        if (vcl < 0)
            break;
        const size_t end = scs[static_cast<size_t>(vcl) + 1];
        out.emplace_back(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(end));
        buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(end));
        ++emitted;
    }
    return emitted;
}

namespace {

bool annexbContainsVcl(const uint8_t* p, size_t n) {
    if (!p || n < 4)
        return false;
    for (size_t i = 0; i + 3 < n;) {
        const size_t sl = annexbStartLen(p, n, i);
        if (!sl) {
            ++i;
            continue;
        }
        const int t = annexbNalType(p, n, i);
        if (t == 1 || t == 5)
            return true;
        i += sl;
    }
    return false;
}

}  // namespace

struct HighCabacHostTranscode::Impl {
    int inWrite = -1;
    int outRead = -1;
    int errRead = -1;
    pid_t pid = -1;
    std::atomic<bool> stopping{false};
    std::atomic<bool> failed{false};
    std::atomic<size_t> bytesOut{0};
    std::thread reader;
    std::mutex mu;
    std::condition_variable cv;
    std::vector<uint8_t> acc;
    std::deque<std::vector<uint8_t>> aus;
    std::string errTail;
    size_t idleSeen = 0;
    int idleStrikes = 0;

    ~Impl() {
        stopping.store(true);
        if (reader.joinable())
            reader.join();
    }

    void noteErr(const uint8_t* p, size_t n) {
        if (!p || n == 0 || errTail.size() >= 240)
            return;
        const size_t room = 240 - errTail.size();
        errTail.append(reinterpret_cast<const char*>(p), n < room ? n : room);
    }

    // 107 held the first encoded frame until the next start code, then the
    // 48-AU cap killed the encoder. A quiet VCL is a finished frame.
    void emitIdleAu() {
        if (acc.empty() || !annexbContainsVcl(acc.data(), acc.size()))
            return;
        aus.push_back(std::move(acc));
        acc.clear();
        idleSeen = 0;
        idleStrikes = 0;
        cv.notify_all();
    }

    void readerMain() {
        uint8_t tmp[8192];
        while (!stopping.load()) {
            pollfd pfds[2]{};
            pfds[0].fd = outRead;
            pfds[0].events = POLLIN;
            nfds_t nfd = 1;
            if (errRead >= 0) {
                pfds[1].fd = errRead;
                pfds[1].events = POLLIN;
                nfd = 2;
            }
            const int pr = ::poll(pfds, nfd, 100);
            if (pr < 0) {
                if (errno == EINTR)
                    continue;
                failed.store(true);
                cv.notify_all();
                break;
            }
            if (pr == 0) {
                std::lock_guard<std::mutex> lock(mu);
                if (!acc.empty() && acc.size() == idleSeen) {
                    if (++idleStrikes >= 2)
                        emitIdleAu();
                } else {
                    idleSeen = acc.size();
                    idleStrikes = 0;
                }
                continue;
            }
            if (errRead >= 0 && (pfds[1].revents & (POLLIN | POLLHUP))) {
                uint8_t etmp[256];
                const ssize_t en = ::read(errRead, etmp, sizeof(etmp));
                if (en > 0) {
                    std::lock_guard<std::mutex> lock(mu);
                    noteErr(etmp, static_cast<size_t>(en));
                }
            }
            if (!(pfds[0].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            const ssize_t n = ::read(outRead, tmp, sizeof(tmp));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                failed.store(true);
                cv.notify_all();
                break;
            }
            if (n == 0) {
                std::lock_guard<std::mutex> lock(mu);
                emitIdleAu();
                failed.store(true);
                cv.notify_all();
                break;
            }
            bytesOut.fetch_add(static_cast<size_t>(n));
            std::lock_guard<std::mutex> lock(mu);
            acc.insert(acc.end(), tmp, tmp + n);
            idleSeen = acc.size();
            idleStrikes = 0;
            std::vector<std::vector<uint8_t>> got;
            extractCompleteAnnexbAus(acc, got);
            for (auto& au : got)
                aus.push_back(std::move(au));
            if (!aus.empty())
                cv.notify_all();
        }
    }
};

HighCabacHostTranscode::HighCabacHostTranscode() = default;

HighCabacHostTranscode::~HighCabacHostTranscode() { stop(); }

bool HighCabacHostTranscode::active() const { return impl_ != nullptr && impl_->pid > 0; }

bool HighCabacHostTranscode::start(const std::string& ffmpegPath, bool baselineOut) {
    stop();
    if (ffmpegPath.empty() || ::access(ffmpegPath.c_str(), X_OK) != 0)
        return false;
    // Build the argv before fork. 182 L6 logged the Baseline transcode line and
    // then "terminate called without an active exception": the child was still
    // a multithreaded C++ process and allocated the argv after fork.
    const auto args = baselineOut ? baselineHostFfmpegArgs(ffmpegPath)
                                  : highCabacFfmpegArgs(ffmpegPath);
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& s : args)
        argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (::pipe(inPipe) != 0 || ::pipe(outPipe) != 0 || ::pipe(errPipe) != 0) {
        if (inPipe[0] >= 0) { ::close(inPipe[0]); ::close(inPipe[1]); }
        if (outPipe[0] >= 0) { ::close(outPipe[0]); ::close(outPipe[1]); }
        if (errPipe[0] >= 0) { ::close(errPipe[0]); ::close(errPipe[1]); }
        return false;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(inPipe[0]); ::close(inPipe[1]);
        ::close(outPipe[0]); ::close(outPipe[1]);
        ::close(errPipe[0]); ::close(errPipe[1]);
        return false;
    }
    if (pid == 0) {
        ::dup2(inPipe[0], STDIN_FILENO);
        ::dup2(outPipe[1], STDOUT_FILENO);
        ::dup2(errPipe[1], STDERR_FILENO);
        ::close(inPipe[0]); ::close(inPipe[1]);
        ::close(outPipe[0]); ::close(outPipe[1]);
        ::close(errPipe[0]); ::close(errPipe[1]);
        for (int fd = 3; fd < 256; ++fd)
            ::close(fd);
        ::execv(argv[0], argv.data());
        _exit(127);
    }
    ::close(inPipe[0]);
    ::close(outPipe[1]);
    ::close(errPipe[1]);
    impl_ = new Impl();
    impl_->inWrite = inPipe[1];
    impl_->outRead = outPipe[0];
    impl_->errRead = errPipe[0];
    impl_->pid = pid;
    impl_->reader = std::thread([this] {
        try {
            impl_->readerMain();
        } catch (...) {
            impl_->failed.store(true);
            impl_->cv.notify_all();
        }
    });
    return true;
}

void HighCabacHostTranscode::endInput() {
    if (!impl_ || impl_->inWrite < 0)
        return;
    ::close(impl_->inWrite);
    impl_->inWrite = -1;
}

bool HighCabacHostTranscode::push(const uint8_t* p, size_t n) {
    if (!impl_ || impl_->inWrite < 0 || !p || n == 0)
        return false;
    size_t off = 0;
    while (off < n) {
        const ssize_t w = ::write(impl_->inWrite, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        off += static_cast<size_t>(w);
    }
    return true;
}

HighCabacHostTranscode::Pop HighCabacHostTranscode::pop(std::vector<uint8_t>& out, int timeoutMs) {
    out.clear();
    if (!impl_)
        return Pop::Failed;
    std::unique_lock<std::mutex> lock(impl_->mu);
    impl_->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        return !impl_->aus.empty() || impl_->failed.load();
    });
    if (!impl_->aus.empty()) {
        out = std::move(impl_->aus.front());
        impl_->aus.pop_front();
        return Pop::Got;
    }
    if (impl_->failed.load())
        return Pop::Failed;
    return Pop::NeedMore;
}

size_t HighCabacHostTranscode::bytesOut() const {
    return impl_ ? impl_->bytesOut.load() : 0;
}

std::string HighCabacHostTranscode::stderrText() const {
    if (!impl_)
        return {};
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->errTail;
}

void HighCabacHostTranscode::stop() {
    if (!impl_)
        return;
    impl_->stopping.store(true);
    if (impl_->inWrite >= 0) {
        ::close(impl_->inWrite);
        impl_->inWrite = -1;
    }
    if (impl_->pid > 0)
        ::kill(impl_->pid, SIGTERM);
    impl_->cv.notify_all();
    if (impl_->reader.joinable())
        impl_->reader.join();
    if (impl_->outRead >= 0) {
        ::close(impl_->outRead);
        impl_->outRead = -1;
    }
    if (impl_->errRead >= 0) {
        ::close(impl_->errRead);
        impl_->errRead = -1;
    }
    if (impl_->pid > 0) {
        ::waitpid(impl_->pid, nullptr, 0);
        impl_->pid = -1;
    }
    delete impl_;
    impl_ = nullptr;
}

namespace {

// 240p letterbox/crop: kPlex240p* + plex240pDdrFrameGeometry live in
// ddr_frame_layout.hpp; ddrFrameGeometryForPresentedSize(640, 240) selects them.

bool setReuse(int fd) {
    int on = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == 0;
}

// Prevent forked ffmpeg/sh from inheriting companion sockets (would hold :3005).
void setCloexec(int fd) {
    int fl = fcntl(fd, F_GETFD);
    if (fl >= 0)
        fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

struct RasterHeightRow {
    const char* mode;
    int active_height;
    int pic_height;
};
constexpr RasterHeightRow kRasterHeights[] = {
    {"240p", 240, 0},
    {"480p", 480, 0},
    {"480i", 480, 240},
};
const RasterHeightRow* rasterHeightFor(const std::string& mode) {
    for (const auto& row : kRasterHeights)
        if (mode == row.mode)
            return &row;
    return nullptr;
}

bool subscriberHostIsLoopback(const std::string& host) {
    std::string h = host;
    for (char& c : h)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h == "127.0.0.1" || h == "localhost" || h == "::1" || h == "0.0.0.0" ||
           h.rfind("127.", 0) == 0;
}

std::string queryParam(const std::string& req, const char* key) {
    const std::string k = std::string(key) + "=";
    auto pos = req.find(k);
    if (pos == std::string::npos)
        return {};
    pos += k.size();
    auto end = req.find_first_of(" &\r\n", pos);
    return req.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

// MPX_COMPANION_POLL_DIAG summary of a timeline reply: the MediaContainer commandID and
// the video Timeline's state/time/duration/key/containerKey/server/queue item.
std::string timelineDiagSummary(const std::string& xml) {
    auto attr = [](const std::string& elem, const char* name) {
        const std::string k = std::string(" ") + name + "=\"";
        const auto p = elem.find(k);
        if (p == std::string::npos)
            return std::string("-");
        const auto b = p + k.size();
        const auto e = elem.find('"', b);
        return e == std::string::npos ? std::string("-") : elem.substr(b, e - b);
    };
    std::string out;
    const auto mc = xml.find("<MediaContainer");
    if (mc != std::string::npos)
        out += "reply_cmd=" + attr(xml.substr(mc, xml.find('>', mc) - mc), "commandID");
    const auto tl = xml.find("<Timeline type=\"video\"");
    if (tl != std::string::npos) {
        const std::string elem = xml.substr(tl, xml.find("/>", tl) - tl);
        for (const char* name : {"state", "time", "duration", "key", "containerKey",
                                 "machineIdentifier", "playQueueItemID", "location",
                                 "address", "port", "protocol", "audioStreamID",
                                 "subtitleStreamID", "videoDecision"})
            out += std::string(" ") + name + "=" + attr(elem, name);
    }
    return out;
}

std::string pctDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() &&
            std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            out.push_back(static_cast<char>(std::strtol(in.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else if (in[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

std::string asciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trimOws(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t'))
        ++begin;
    size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t'))
        --end;
    return value.substr(begin, end - begin);
}

std::string headerValue(const std::string& req, const char* name) {
    const std::string wanted = asciiLower(name);
    auto line = req.find("\r\n");
    if (line == std::string::npos)
        return {};
    line += 2;
    while (line < req.size()) {
        const auto end = req.find("\r\n", line);
        if (end == std::string::npos || end == line)
            return {};
        const auto colon = req.find(':', line);
        if (colon != std::string::npos && colon < end &&
            asciiLower(req.substr(line, colon - line)) == wanted) {
            return trimOws(req.substr(colon + 1, end - colon - 1));
        }
        line = end + 2;
    }
    return {};
}

std::string controllerId(const std::string& req) {
    std::string id = pctDecode(queryParam(req, "X-Plex-Client-Identifier"));
    if (id.empty())
        id = pctDecode(queryParam(req, "clientIdentifier"));
    if (id.empty())
        id = pctDecode(headerValue(req, "X-Plex-Client-Identifier"));
    return id;
}

uint16_t callbackPort(const std::string& req) {
    const std::string raw = queryParam(req, "port");
    if (raw.empty())
        return 0;
    char* end = nullptr;
    const unsigned long value = std::strtoul(raw.c_str(), &end, 10);
    if (!end || *end != '\0' || value == 0 || value > 65535)
        return 0;
    return static_cast<uint16_t>(value);
}

bool sendAll(int fd, const std::string& bytes) {
    size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t n =
            ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

bool decimalCommandId(const std::string& value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isdigit(ch) != 0;
           });
}

std::string normalizedDecimal(const std::string& value) {
    const auto first = value.find_first_not_of('0');
    return first == std::string::npos ? "0" : value.substr(first);
}

bool commandIsNewerOrEqual(const std::string& candidate, const std::string& current) {
    if (current.empty())
        return true;
    if (!decimalCommandId(candidate) || !decimalCommandId(current))
        return true;
    const std::string left = normalizedDecimal(candidate);
    const std::string right = normalizedDecimal(current);
    if (left.size() != right.size())
        return left.size() > right.size();
    return left >= right;
}

// SEC-M5: the validated request Origin, echoed in Access-Control-Allow-Origin (never "*"). Set by httpLoop for
// the request it is answering; sendHttp is only called on that thread.
thread_local std::string g_corsOrigin;

void sendHttp(int fd, const std::string& clientIdentifier, int code, const char* ctype,
              const std::string& body,
              const std::string& allowHeaders = std::string()) {
    const char* status = "Error";
    switch (code) {
    case 200:
        status = "OK";
        break;
    case 400:
        status = "Bad Request";
        break;
    case 404:
        status = "Not Found";
        break;
    case 405:
        status = "Method Not Allowed";
        break;
    case 202:
        status = "Accepted";
        break;
    case 403:
        status = "Forbidden";
        break;
    case 409:
        status = "Conflict";
        break;
    case 503:
        status = "Service Unavailable";
        break;
    case 408:
        status = "Request Timeout";
        break;
    case 413:
        status = "Payload Too Large";
        break;
    }
    std::ostringstream response;
    response << "HTTP/1.1 " << code << " " << status << "\r\n"
             << "Content-Type: " << ctype << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n"
#ifndef MUT_PSW_NO_FRAME_HEADERS
             << ((std::strncmp(ctype, "text/html", 9) == 0 && (ctype[9] == '\0' || ctype[9] == ';'))
                     ? "X-Frame-Options: DENY\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: "
                       "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'\r\n"
                     : "")
#endif
             << "X-Plex-Client-Identifier: " << clientIdentifier << "\r\n"
#ifdef MUT_COMPANION_CORS_STAR
             << "Access-Control-Allow-Origin: *\r\n"
#else
             << (g_corsOrigin.empty() ? std::string()
                                      : "Access-Control-Allow-Origin: " + g_corsOrigin + "\r\nVary: Origin\r\n"
                                        "Access-Control-Allow-Private-Network: true\r\n")
#endif
             << "Access-Control-Allow-Headers: "
             << (allowHeaders.empty()
                     ? "X-Plex-Token, X-Plex-Client-Identifier, "
                       "X-Plex-Target-Client-Identifier, X-Plex-Session-Identifier, "
                       "X-Plex-Product, X-Plex-Version, X-Plex-Device, "
                       "X-Plex-Device-Name, X-Plex-Platform, "
                       "X-Plex-Platform-Version, X-Plex-Model, "
                       "X-Plex-Provider-Version, X-Plex-Text-Format, "
                       "X-Plex-Language, X-Plex-Features, X-Plex-Drm, "
                       "Content-Type, Accept"
                     : allowHeaders)
             << "\r\n"
             << "Access-Control-Allow-Methods: GET, POST, PUT, OPTIONS\r\n"
             << "Access-Control-Expose-Headers: " << kCorsExposeHeaders << "\r\n"
             << "Access-Control-Max-Age: 600\r\n\r\n"
             << body;
    (void)sendAll(fd, response.str());
}

enum class HttpHeaderRead { Ok, Timeout, TooLarge, Closed, Error };

HttpHeaderRead recvHttpHeaders(int fd, std::string& out, size_t maxBytes = 16384,
                               int timeoutMs = 5000) {
    out.clear();
    char chunk[2048];
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (out.find("\r\n\r\n") == std::string::npos) {
        if (out.size() >= maxBytes)
            return HttpHeaderRead::TooLarge;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return HttpHeaderRead::Timeout;
        const auto remaining =
            std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval timeout{};
        timeout.tv_sec = static_cast<time_t>(remaining.count() / 1000000);
        timeout.tv_usec = static_cast<suseconds_t>(remaining.count() % 1000000);
        const int selected = select(fd + 1, &rfds, nullptr, nullptr, &timeout);
        if (selected == 0)
            return HttpHeaderRead::Timeout;
        if (selected < 0) {
            if (errno == EINTR)
                continue;
            return HttpHeaderRead::Error;
        }
        const size_t available = maxBytes - out.size();
        const ssize_t n = recv(fd, chunk, std::min(available, sizeof(chunk)), 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return HttpHeaderRead::Error;
        }
        if (n == 0)
            return HttpHeaderRead::Closed;
        out.append(chunk, static_cast<size_t>(n));
    }
    return HttpHeaderRead::Ok;
}

// Read exactly n more bytes, or fail. Used only by POST /misterplex/update, and only up to
// phone_status::kUpdateBodyCap, so a status POST cannot stall the companion thread.
bool readExact(int fd, std::string& out, size_t n, int timeoutMs) {
    out.clear();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (out.size() < n) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{};
        tv.tv_sec = static_cast<time_t>(left.count() / 1000000);
        tv.tv_usec = static_cast<suseconds_t>(left.count() % 1000000);
        const int selected = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (selected < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (selected == 0)
            return false;
        char buf[256];
        const size_t want = std::min(n - out.size(), sizeof buf);
        const ssize_t got = ::recv(fd, buf, want, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (got == 0)
            return false;
        out.append(buf, static_cast<size_t>(got));
    }
    return true;
}

// Companion offset/viewOffset are milliseconds (PMS universal offset= is seconds).
// viewOffset is the Plex Web resume position. playMedia often also sends offset=0,
// and queryParam("offset") matches the suffix of viewOffset, so the zero used to
// hide a real resume (F242 094: ACK time stayed 0, playback followed the library offset).
int64_t parseOffsetMs(const std::string& req, bool* present) {
    if (present)
        *present = false;
    auto viewOff = queryParam(req, "viewOffset");
    if (!viewOff.empty()) {
        if (present)
            *present = true;
        return std::atoll(viewOff.c_str());
    }
    for (const char* name : {"offset", "time", "startTimeOffset"}) {
        auto off = queryParam(req, name);
        if (!off.empty()) {
            if (present)
                *present = true;
            return std::atoll(off.c_str());
        }
    }
    return 0;
}

static void trimTrailingSlashes(std::string& s) {
    while (!s.empty() && s.back() == '/')
        s.pop_back();
}

} // namespace

PlayRequest parsePlayRequest(const std::string& req) {
    PlayRequest pr;
    pr.key = pctDecode(queryParam(req, "key"));
    pr.containerKey = pctDecode(queryParam(req, "containerKey"));
    pr.playQueueItemId = queryParam(req, "playQueueItemID");
    pr.playQueueVersion = queryParam(req, "playQueueVersion");
    pr.ratingKey = queryParam(req, "ratingKey");
    if (pr.ratingKey.empty())
        pr.ratingKey = ratingKeyFromKey(pr.key);
    pr.address = pctDecode(queryParam(req, "address"));
    pr.protocol = queryParam(req, "protocol");
    pr.port = queryParam(req, "port");
    // SEC-M2: the target must be a plain http(s) host[:port] (no path/userinfo/query, no link-local/metadata address);
    // otherwise drop it so the key resolves against the configured server instead of an attacker-chosen one.
#ifndef MUT_COMPANION_NO_TARGET_GUARD
    if (!guard::playTargetOk(pr.address, pr.protocol, pr.port)) {
        pr.address.clear();
        pr.protocol.clear();
        pr.port.clear();
        pr.targetRejected = true;
    }
#endif
#ifdef MPX_480P_BITRATE_FLOOR
    // Detect 480p via dimension query params (PMS web cast sends resolution=480).
    const std::string resolution = queryParam(req, "resolution");
    if (resolution == "480") {
        pr.is480p = true;
        pr.minBitrateFloorKbps = MPX_480P_BITRATE_FLOOR_KBPS;
    }
#endif
    // Prefer lowercase token= (Plex Web cast). Do not substring-match inside
    // other keys. Always percent-decode — cast tokens may be URL-encoded.
    pr.token = pctDecode(queryParam(req, "token"));
    if (pr.token.empty())
        pr.token = pctDecode(queryParam(req, "X-Plex-Token"));
    if (pr.token.empty())
        pr.token = pctDecode(headerValue(req, "X-Plex-Token"));
    pr.serverMachineId = queryParam(req, "machineIdentifier");
    pr.offsetMs = parseOffsetMs(req, &pr.offsetPresent);
    auto parseStreamId = [&](const char* key, bool allowZero, bool& has, int& id) {
        const std::string raw = queryParam(req, key);
        if (raw.empty())
            return;
        errno = 0;
        char* end = nullptr;
        const long long n = std::strtoll(raw.c_str(), &end, 10);
        if (end && end != raw.c_str() && *end == '\0' && errno != ERANGE && n >= 0 &&
            n <= static_cast<long long>(INT_MAX) && (allowZero || n > 0)) {
            has = true;
            id = static_cast<int>(n);
        }
    };
    parseStreamId("subtitleStreamID", true, pr.hasSubtitleStreamId, pr.subtitleStreamId);
    parseStreamId("audioStreamID", false, pr.hasAudioStreamId, pr.audioStreamId);
    if (pr.containerKey.find("/playQueues/") != std::string::npos) {
        auto rest = pr.containerKey.substr(std::string("/playQueues/").size());
        auto q = rest.find('?');
        pr.playQueueId = (q == std::string::npos) ? rest : rest.substr(0, q);
        trimTrailingSlashes(pr.playQueueId);
    } else {
        // Never treat containerKey=/library/metadata/N as a queue (poisons Web NY→isOpen).
        auto pq = queryParam(req, "playQueueID");
        if (!pq.empty()) {
            pr.playQueueId = pq;
            trimTrailingSlashes(pr.playQueueId);
        }
        if (pr.containerKey.find("/playQueues/") == std::string::npos &&
            pr.containerKey.find("/library/") != std::string::npos)
            pr.containerKey.clear();
    }
    return pr;
}

void applyRasterLetterbox(PlayRequest& pr, int rasterLines, int codedWidth, int codedHeight,
                          int cropLeft, int cropRight, int cropTop, int cropBottom) {
    pr.cropLeft = cropLeft;
    pr.cropRight = cropRight;
    pr.cropTop = cropTop;
    pr.cropBottom = cropBottom;
    pr.letterbox = false;
    pr.active_height = 0;
    if (codedWidth <= 0 || codedHeight <= 0)
        return;
    const auto p = streamRasterPlacementN(rasterLines, codedWidth, codedHeight, cropLeft,
                                          cropRight, cropTop, cropBottom);
    if (!p.valid)
        return;
    pr.letterbox = p.letterbox;
    pr.active_height = p.display_height;
}

void apply240pLetterbox(PlayRequest& pr, int codedWidth, int codedHeight, int cropLeft,
                        int cropRight, int cropTop, int cropBottom) {
    applyRasterLetterbox(pr, kSpsLetterboxRasterLines240, codedWidth, codedHeight, cropLeft,
                         cropRight, cropTop, cropBottom);
}

void apply480pLetterbox(PlayRequest& pr, int codedWidth, int codedHeight, int cropLeft,
                        int cropRight, int cropTop, int cropBottom) {
    applyRasterLetterbox(pr, kSpsLetterboxRasterLines, codedWidth, codedHeight, cropLeft, cropRight,
                         cropTop, cropBottom);
}

void apply480iLetterbox(PlayRequest& pr, int codedWidth, int codedHeight, int cropLeft,
                        int cropRight, int cropTop, int cropBottom) {
    pr.is480i = true;
    applyRasterLetterbox(pr, kSpsLetterboxRasterLines, codedWidth, codedHeight, cropLeft, cropRight,
                         cropTop, cropBottom);
}

void applyRasterActiveHeight(PlayRequest& pr, int rasterCap, int codedWidth, int codedHeight,
                             int cropLeft, int cropRight, int cropTop, int cropBottom) {
    (void)cropLeft;
    (void)cropRight;
    pr.active_height = 0;
    pr.pic_height = 0;
    pr.crop_height = 0;
    if (codedWidth <= 0 || codedHeight <= 0)
        return;
    if (cropTop < 0 || cropBottom < 0)
        return;
    pr.pic_height = codedHeight;
    pr.crop_height = cropTop + cropBottom;
    const int display_h = codedHeight - pr.crop_height;
    if (display_h < 0)
        return;
    pr.active_height = (rasterCap > 0 && display_h >= rasterCap) ? rasterCap : display_h;
}

void apply240pActiveHeight(PlayRequest& pr, int codedWidth, int codedHeight, int cropLeft,
                           int cropRight, int cropTop, int cropBottom) {
    applyRasterActiveHeight(pr, 240, codedWidth, codedHeight, cropLeft, cropRight, cropTop,
                            cropBottom);
}

TitleDirectPolicy decideTitleDirectPolicy(const TitleStreamFacts& facts, const char** why) {
    const std::string codec = asciiLower(facts.codec);
    const std::string profile = asciiLower(facts.profile);
    auto setWhy = [why](const char* reason) {
        if (why)
            *why = reason;
    };
    if (codec != "h264") {
        setWhy("codec");
        return TitleDirectPolicy::Proxy;
    }
    if (profile != "baseline" && profile != "constrained baseline") {
        setWhy("profile");
        return TitleDirectPolicy::Proxy;
    }
    if (facts.refFrames > 1) {
        setWhy("refs");
        return TitleDirectPolicy::Proxy;
    }
    if (facts.interlaced) {
        setWhy("interlaced");
        return TitleDirectPolicy::Proxy;
    }
    if (facts.level > 30) {
        setWhy("level");
        return TitleDirectPolicy::Proxy;
    }
    if (facts.height > 480 || (facts.width != 0 && facts.width > 720)) {
        setWhy("size");
        return TitleDirectPolicy::Proxy;
    }
    setWhy("direct");
    return TitleDirectPolicy::Direct;
}

std::string titlePlayPolicy(const PlayRequest& pr) {
    const std::string& key = pr.key;
    if (key.rfind("http://", 0) == 0 || key.rfind("https://", 0) == 0)
        return "DIRECT";
    if (!key.empty() && key[0] == '/' && key.rfind("/library", 0) != 0 &&
        key.rfind("/playQueues", 0) != 0)
        return "DIRECT";
    return "PROXY";
}

// Track whether the last stop was user-initiated (vs decoder/process death).
bool userStop_ = false;

// 480i fps_gate: named min_fps floor (~24) is enforced in plex_resolve.cpp
// (fpga480iMinFpsOk vs 24000/1001). PlayRequest has no fps/raster, so bindMedia
// cannot apply the floor.

// 480i bitrate floor (kbps): kBitrateFloor480iKbps = 2000, same as the 480p
// AU-safe / validateWeakLadder min. Applied via fpgaAuSafeTier480 so a named
// 480i row cannot take the 240p 750 floor.

// 240p fps_gate: named min_fps floor (~24) is enforced in plex_resolve.cpp
// (fpga240pMinFpsOk vs 24000/1001; frame_rate 24/1 and 24000/1001 pass). PlayRequest
// has no fps/raster, so playMedia cannot apply the floor.

// 240p bitrate floor (kbps): kBitrateFloor240pKbps = 750, same as the AU-safe
// 240p min. Applied in fpgaAuSafeClamp / validateWeakLadder / lowerPmsLadder
// so 240p rows cannot undershoot; 480p/480i 2000 rungs stay put.

// 480p fps_gate: named min_fps floor (~24) is enforced in plex_resolve.cpp
// (fpga480pMinFpsOk vs 24000/1001; frame_rate 24/1 and 24000/1001 pass). PlayRequest
// has no fps/raster, so playMedia cannot apply the floor.

// Which cast request may refresh the PMS auth token (timeline + seek replay resolve).
// playMedia carries the PLAYING server's token as token= (Plex Web: the remote plex.direct
// transient token). Transport controls (seekTo, playback/*, command) only carry the
// CONTROLLER's own X-Plex-Token (query or header) — the token Plex Web holds for the server
// it browses, which a remote/shared server rejects.  lab Quantum Leap scrub (mpxd-seek-1556):
// seekTo replaced the transient token with the controller's, so the seek's re-resolve got a
// thin (401) metadata answer ("requires numeric source frameRate"), CLOSED-STOP, black
// screen, and /:/timeline went 401. Controls therefore refresh only from an explicit token=.
std::string castControlRefreshToken(const std::string& req) {
    if (req.find("/player/") == std::string::npos)
        return {};
    const bool isPoll =
        req.find("/player/timeline/poll") != std::string::npos ||
        req.find("/player/timeline/subscribe") != std::string::npos ||
        req.find("/player/timeline/unsubscribe") != std::string::npos ||
        req.find("/player/proxy/timeline") != std::string::npos;
    if (isPoll)
        return {};
    const bool isPlayMedia = req.find("playMedia") != std::string::npos;
    const bool castControl =
        isPlayMedia ||
        req.find("/player/playback/") != std::string::npos ||
        req.find("seekTo") != std::string::npos ||
        req.find("/player/command") != std::string::npos;
    if (!castControl)
        return {};
    std::string tok = pctDecode(queryParam(req, "token"));
#ifdef MUT_SEEK_TAKES_CONTROLLER_TOKEN   // test-only mutant: the pre-fix rule
    const bool controllerTokenOk = true;
#else
    const bool controllerTokenOk = isPlayMedia;
#endif
    if (tok.empty() && controllerTokenOk)
        tok = pctDecode(queryParam(req, "X-Plex-Token"));
    if (tok.empty() && controllerTokenOk)
        tok = pctDecode(headerValue(req, "X-Plex-Token"));
    return tok;
}

bool tokenRefreshesActiveSession(const std::string& req) {
#ifdef MUT_F1_PLAYMEDIA_REFRESHES
    return !castControlRefreshToken(req).empty();
#else
    if (req.find("playMedia") != std::string::npos)
        return false;
    return !castControlRefreshToken(req).empty();
#endif
}

void Companion::log(const std::string& s) const {
    // SyncedLogAssembler::push redacts via redactUrlTokens before any thread prints.
    const std::vector<std::string> lines = logAsm_.push(s);
    for (const std::string& line : lines) {
        if (log_)
            log_(line);
        else
            std::fprintf(stderr, "%s\n", line.c_str());
    }
}

std::string Companion::xmlEsc(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&':
            o += "&amp;";
            break;
        case '<':
            o += "&lt;";
            break;
        case '>':
            o += "&gt;";
            break;
        case '"':
            o += "&quot;";
            break;
        default:
            o += c;
        }
    }
    return o;
}

std::string Companion::lanIp() const {
    // Prefer a real interface address. The UDP-connect-to-1.1.1.1 trick returns
    // 127.0.0.1 on WiFi-only MiSTers whose default route is missing or local-only
    // (user saw cast target "127.0.0.1" on  lab).
    ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == 0) {
        std::string best;
        for (ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                continue;
            if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
                continue;
            char buf[64] = {};
            auto* sin = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
            if (!inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)))
                continue;
            if (std::strncmp(buf, "127.", 4) == 0)
                continue;
            // Prefer RFC1918 LAN over link-local.
            const bool priv = (std::strncmp(buf, "10.", 3) == 0) ||
                              (std::strncmp(buf, "192.168.", 8) == 0) ||
                              (std::strncmp(buf, "172.", 4) == 0);
            if (best.empty() || priv) {
                best = buf;
                if (priv)
                    break;
            }
        }
        freeifaddrs(ifaddr);
        if (!best.empty())
            return best;
    }
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return "127.0.0.1";
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "1.1.1.1", &a.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        close(fd);
        return "127.0.0.1";
    }
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len);
    close(fd);
    char buf[64];
    inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
    return buf;
}

std::string Companion::gdmPayload() const {
    return gdmPayload(false);
}

std::string Companion::gdmPayload(bool hello) const {
    std::ostringstream o;
    o << (hello ? "HELLO * HTTP/1.0\r\n" : "HTTP/1.0 200 OK\r\n")
      << "Content-Type: plex/media-player\r\n"
      << "Name: " << name_ << "\r\n"
      << "Port: " << port_ << "\r\n"
      << "Product: MiSTerPlex\r\n"
      << "Version: 0.4.1\r\n"
      << "Protocol: plex\r\n"
      << "Protocol-Version: 1\r\n"
      << "Protocol-Capabilities: " << kCompanionProtocolCapabilities << "\r\n"
      << "Device-Class: stb\r\n"
      << "Resource-Identifier: " << machineId_ << "\r\n"
      << "\r\n";
    return o.str();
}

std::string Companion::resourcesXml(const std::string& playerIp) const {
    // Player address is the IPv4 this HTTP connection arrived on (dual-home
    // boxes must not advertise one iface's address to a client on another).
    // Timeline.address stays the PMS — see reachableTimelinePms / setTimelinePmsBase.
    const std::string ip = playerIp.empty() ? lanIp() : playerIp;
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      << "<MediaContainer>"
      << "<Player title=\"" << xmlEsc(name_) << "\" product=\"MiSTerPlex\" "
      << "protocol=\"plex\" protocolVersion=\"1\" "
      << "protocolCapabilities=\"" << kCompanionProtocolCapabilities << "\" "
      << "provides=\"player\" "
      << "deviceClass=\"stb\" machineIdentifier=\"" << xmlEsc(machineId_) << "\" "
      << "address=\"" << xmlEsc(ip) << "\" port=\"" << port_ << "\" "
      << "version=\"0.4.1\"/>"
      << "</MediaContainer>";
    return o.str();
}

bool Companion::endedIdleLocked() const {
#ifdef MUT_RECAST_NO_IDLE
    return false;
#else
    return terminalStop_ && state_ == "ended" &&
           std::chrono::steady_clock::now() - endedAt_ >= std::chrono::milliseconds(endedIdleMs_);
#endif
}

std::string Companion::containerOf(const std::string& containerKey, const std::string& playQueueId) {
    std::string container;
    // Prefer the cast containerKey so a trailing slash before ?own= is not lost when
    // playQueueID is also present (Web often sends both).
    if (!containerKey.empty() && containerKey.find("/playQueues/") != std::string::npos) {
        container = containerKey;
        const auto q = container.find('?');
        if (q != std::string::npos)
            container = container.substr(0, q);
        if (container.find("own=") == std::string::npos)
            container += "?own=1";
    } else if (!playQueueId.empty()) {
        container = "/playQueues/" + playQueueId + "?own=1";
    }
    return container;
}

std::string Companion::timelineXml(const std::string& commandId,
                                   const std::string& recipientId,
                                   const std::string& recipientPeer) const {
    std::string ownerId;
    std::string ownerHost;
    {
        std::lock_guard<std::mutex> subscribers(subscriberMu_);
        ownerId = castOwnerId_;
        ownerHost = castOwnerHost_;
    }
    std::lock_guard<std::mutex> lock(mu_);
    const std::string effectiveCommandId = commandId.empty() ? "0" : commandId;

    std::string videoState = state_ == "ended" ? "stopped" : state_;
    // castBound_ alone never holds; the whole hold is bounded by holdUntil_.
    const bool holdActive = std::chrono::steady_clock::now() < holdUntil_;
    const bool holdIdle =
        !terminalStop_ && !wantPlay_ && prePlayHold_ && holdActive &&
        (videoState == "stopped" || videoState.empty() || videoState == "buffering");
    if (!terminalStop_ && wantPlay_ && (videoState == "stopped" || videoState.empty()))
        videoState = "buffering";
    else if (holdIdle)
        videoState = "buffering";

    // BUG2-RECAST: a finished queue decays to the unbound idle after endedIdleMs_.
    const bool endedIdle = endedIdleLocked();
    const bool mediaActive =
        !endedIdle && wantPlay_ &&
        (videoState == "playing" || videoState == "paused" || videoState == "buffering" || !pendingKey_.empty());
    // 480i A/V: NTSC field rate is 59.94 Hz (two fields per 29.97 frame), not 29.97p.
    // Raster is process-wide: MPX_FPGA_MODE=480i, or truthy MPX_PRESENT_480I (same
    // test as fpgaRasterNtsc / envFlagWanted). Companion has no resolve height/fps.
    const char* fpgaModeEnv = std::getenv("MPX_FPGA_MODE");
    const bool mode480i = fpgaModeEnv && std::strcmp(fpgaModeEnv, "480i") == 0;
    const char* present480iEnv = std::getenv("MPX_PRESENT_480I");
    const bool present480i =
        present480iEnv && present480iEnv[0] &&
        (present480iEnv[0] == '1' || present480iEnv[0] == 'y' ||
         present480iEnv[0] == 'Y' || std::strcmp(present480iEnv, "on") == 0 ||
         std::strcmp(present480iEnv, "ON") == 0 ||
         std::strcmp(present480iEnv, "yes") == 0 ||
         std::strcmp(present480iEnv, "YES") == 0);
    const bool paired480i = CoreGate::process().pairedRaster() == "480i";
    const bool is480iFieldRate = mode480i || present480i || paired480i;
    if (!terminalStop_ && mediaActive && videoState == "stopped")
        videoState = "buffering";

    const std::string videoLoc = mediaActive ? "fullScreenVideo" : "navigation";
    const char* videoCtrl =
        "playPause,stop,volume,audioStream,subtitleStream,seekTo,skipPrevious,skipNext,"
        "stepBack,stepForward";

    const std::string container = containerOf(pendingContainerKey_, pendingPlayQueueId_);

    int64_t dur = (mediaActive && durationMs_ > 0) ? durationMs_ : 0;
    int64_t reportMs = mediaActive ? std::max<int64_t>(0, timeMs_) : 0;
#ifndef MUT_F3_HOLD_ZERO
    // The post-Stop buffering hold keeps the last known position and the real duration. Zeros here made the
    // controller see "buffering 0/0" for the whole hold (default 8 s) before the stopped state.
    if (holdIdle && !mediaActive && holdDurMs_ > 0) {
        dur = holdDurMs_;
        reportMs = std::min<int64_t>(std::max<int64_t>(0, holdTimeMs_), holdDurMs_);
    }
#endif
    // Plex Web's cast scrubber reads Timeline@viewOffset (issue #23 / #122).
    // MediaContainer@viewOffset alone stays unread by that parser. The attribute
    // tracks the same millisecond clock as time= and playbackTime=, including the
    // post-Stop hold. Pause holds it. A seek plant holds it until live playback
    // passes the plant.
#ifdef MUT_RC1036_FREEZE_OFFSET
    const int64_t timelineViewMs = 0;
#else
    const int64_t timelineViewMs = reportMs;
#endif

    std::ostringstream b;
    b << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      << "<MediaContainer machineIdentifier=\"" << xmlEsc(machineId_) << "\" size=\"3\" commandID=\""
      << xmlEsc(effectiveCommandId) << "\" location=\"" << videoLoc << "\" viewOffset=\""
      << reportMs << "\">";

    b << "<Timeline type=\"video\" state=\"" << videoState << "\" time=\"" << reportMs
      << "\" playbackTime=\"" << reportMs
      << "\" viewOffset=\"" << timelineViewMs
      << "\" duration=\"" << dur << "\" ";
#ifdef MUT_PB_NO_SEEKRANGE
    if (false)
#else
    if (dur > 0)
#endif
        b << "seekRange=\"0-" << dur << "\" ";
    if (barTop_ > 0 || barBot_ > 0 || barLeft_ > 0 || barRight_ > 0)
        b << "bar_top=\"" << barTop_ << "\" bar_bot=\"" << barBot_
          << "\" bar_left=\"" << barLeft_ << "\" bar_right=\"" << barRight_ << "\" ";
    // 480i rows: active_height=480 (frame), pic_height=240 (field).
    // 480p/240p rows keep their own active_height and omit the 480i field crop.
    if (rasterMode_ == "480i") {
        b << "active_height=\"" << k480iActiveHeight << "\" ";
        b << "pic_height=\"" << k480iPicHeight << "\" ";
    } else if (activeHeight_ > 0) {
        b << "active_height=\"" << activeHeight_ << "\" ";
    }
    b << "volume=\"" << volume_.load() << "\" mute=\"" << (volume_.load() == 0 ? 1 : 0)
      << "\" controllable=\"" << videoCtrl << "\" location=\""
      << videoLoc << "\" mediaIndex=\"0\" partIndex=\"0\" partCount=\"" << (mediaActive ? 1 : 0) << "\" "
      << "itemType=\"video\" shuffle=\"0\" repeat=\"0\" autoPlay=\"1\" ";
    // Plex Web checks these on every poll (see track_select.hpp). Only while a play is up,
    // and only once the player or a setStreams command has named them.
    if (mediaActive && streamsKnown_)
        b << timelineStreamAttrs(true, audioStreamId_, subtitleStreamId_);
    // plex-mpv-shim omits videoDecision. We only emit it once resolve knows
    // the real session (direct vs transcode). Lying "direct" on a transcode
    // play is a client-side inconsistency (s4: transcode=1 + videoDecision=direct).
    if (mediaActive && !playDecision_.empty()) {
        b << "videoDecision=\"" << xmlEsc(playDecision_) << "\" audioDecision=\""
          << xmlEsc(playDecision_) << "\" ";
        if (playDecision_ == "direct" && streamsKnown_ && subtitleStreamId_ > 0)
            b << "subtitleDecision=\"direct\" ";
    }
    if (is480iFieldRate)
        b << "fieldRate=\"59.94\" ";

    if (mediaActive && !pendingKey_.empty()) {
        const std::string srvId = !serverMachineId_.empty() ? serverMachineId_ : machineId_;
        b << "machineIdentifier=\"" << xmlEsc(srvId) << "\" ";
        if (!serverHost_.empty()) {
            b << "protocol=\"" << xmlEsc(serverProto_) << "\" address=\"" << xmlEsc(serverHost_)
              << "\" port=\"" << xmlEsc(serverPort_) << "\" ";
        }
        // Plex remote-control spec: token identifies the playing server so the
        // controller (Plex Web) can fetch metadata and apply time= updates.
        // Only the cast owner at this peer receives it. A copied client id from
        // another IP does not. An empty owner id or owner IP matches nobody.
        // Evaluated from the current owner on every send.
        bool peerOk = !recipientPeer.empty() && recipientPeer == ownerHost;
#ifdef MUT_TIMELINE_TOKEN_IGNORE_PEER
        peerOk = true;
#endif
        bool tokenForRecipient = !recipientId.empty() && recipientId == ownerId && peerOk &&
                                 !pendingToken_.empty();
#ifdef MUT_TIMELINE_TOKEN_ANY_CLIENT
        tokenForRecipient = !pendingToken_.empty();
#endif
#ifdef MUT_TIMELINE_OMIT_CAST_TOKEN
        tokenForRecipient = false;
#endif
        if (tokenForRecipient)
            b << "token=\"" << xmlEsc(pendingToken_) << "\" ";
        b << "providerIdentifier=\"com.plexapp.plugins.library\" ";
        b << "key=\"" << xmlEsc(pendingKey_) << "\" ";
#ifdef MUT_PB_NO_CONTAINERKEY
        if (false)
#else
        if (!container.empty())
#endif
            b << "containerKey=\"" << xmlEsc(container) << "\" ";
        if (!pendingRatingKey_.empty())
            b << "ratingKey=\"" << xmlEsc(pendingRatingKey_) << "\" ";
        if (!pendingPlayQueueId_.empty()) {
            b << "playQueueID=\"" << xmlEsc(pendingPlayQueueId_) << "\" ";
            b << "playQueueVersion=\""
              << xmlEsc(pendingPlayQueueVersion_.empty() ? "1" : pendingPlayQueueVersion_)
              << "\" ";
        }
        if (!pendingPlayQueueItemId_.empty())
            b << "playQueueItemID=\"" << xmlEsc(pendingPlayQueueItemId_) << "\" ";
    } else if (!idleKey_.empty()
#ifndef MUT_IDLEKEY_NO_EXPIRY
               && std::chrono::steady_clock::now() < idleKeyUntil_
#endif
#ifdef MUT_IDLEKEY_ALWAYS_BLANK
               && false
#endif
               ) {
        // rc-1034: the post-Stop buffering hold and the final stopped push keep the last item's identity (no
        // token), so a controller can still match the stopped item. Falls back to blank after idleKeyMs_.
        b << "machineIdentifier=\"" << xmlEsc(!serverMachineId_.empty() ? serverMachineId_ : machineId_) << "\" ";
        b << "key=\"" << xmlEsc(idleKey_) << "\" ";
        if (!idleContainer_.empty())
            b << "containerKey=\"" << xmlEsc(idleContainer_) << "\" ";
        if (!idleRatingKey_.empty())
            b << "ratingKey=\"" << xmlEsc(idleRatingKey_) << "\" ";
    } else {
        b << "machineIdentifier=\"" << xmlEsc(machineId_) << "\" ";
    }
    b << "/>";
    // Idle music and photo Timelines, state=stopped: Plex Home Theater and PKC always send all three
    // and plexapi's ClientTimeline / Plex Web look a Timeline up by type (no-proxy commit c).
    for (const char* idleType : {"music", "photo"})
        b << "<Timeline type=\"" << idleType << "\" itemType=\"" << idleType
          << "\" state=\"stopped\" time=\"0\" duration=\"0\" location=\"navigation\" shuffle=\"0\" "
             "repeat=\"0\" volume=\"" << volume_.load() << "\" mute=\"" << (volume_.load() == 0 ? 1 : 0)
          << "\" machineIdentifier=\"" << xmlEsc(machineId_) << "\" />";
    b << "</MediaContainer>";
    return b.str();
}

SetStreamsResult Companion::applyAndEchoStreams(const SetStreamsRequest& asked) {
    SetStreamsResult applied;
    applied.known = asked.hasAudio || asked.hasSubtitle;
    {
        std::lock_guard<std::mutex> lock(mu_);
        applied.audioStreamId = streamsKnown_ ? audioStreamId_ : 0;
        applied.subtitleStreamId = streamsKnown_ ? subtitleStreamId_ : 0;
    }
    if (asked.hasAudio)
        applied.audioStreamId = asked.audioStreamId;
    if (asked.hasSubtitle)
        applied.subtitleStreamId = asked.subtitleStreamId;
    if (onStreams_) {
        try {
            applied = onStreams_(asked);
        } catch (...) {
            log("setStreams handler exception");
        }
    }
    if (applied.known) {
        std::lock_guard<std::mutex> lock(mu_);
        audioStreamId_ = applied.audioStreamId < 0 ? 0 : applied.audioStreamId;
        subtitleStreamId_ = applied.subtitleStreamId < 0 ? 0 : applied.subtitleStreamId;
        streamsKnown_ = true;
    }
    if (!applied.note.empty())
        log("companion: setStreams " + applied.note);
    else if (log_)
        log_("companion: setStreams audio=" + std::to_string(applied.audioStreamId) +
             " subtitle=" + std::to_string(applied.subtitleStreamId));
    return applied;
}

void Companion::noteStreams(int audioStreamId, int subtitleStreamId) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        audioStreamId_ = audioStreamId < 0 ? 0 : audioStreamId;
        subtitleStreamId_ = subtitleStreamId < 0 ? 0 : subtitleStreamId;
        streamsKnown_ = true;
    }
    requestTimelinePush(true);
}

void Companion::setPlayDecision(std::string decision) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        playDecision_ = std::move(decision);
    }
    requestTimelinePush(true);
}

void Companion::bindTimelinePmsLocked(const std::string& address, const std::string& port,
                                      const std::string& protocol) {
    const std::string prevHost = serverHost_;
    const std::string playerLan = !playerLanHint_.empty() ? playerLanHint_ : lanIp();
    // #347: an explicit cast address keeps the timeline on that host (no conf-default gateway rewrite).
    const PlexEndpoint ep =
        castTimelinePms(address, port, protocol, lanPreferredBase_, playerLan);
    serverHost_ = ep.host;
    if (!ep.port.empty())
        serverPort_ = ep.port;
    if (!ep.protocol.empty())
        serverProto_ = ep.protocol;
    else if (!protocol.empty())
        serverProto_ = protocol;
    if (serverHost_ != prevHost || (!address.empty() && serverHost_ != address)) {
        std::string line = "companion: timeline PMS proto=" + serverProto_ + " address=" +
                           serverHost_ + " port=" + serverPort_;
        if (!address.empty() && address != serverHost_)
            line += " from " + address;
        log(line);
    }
}

void Companion::setTimelinePmsBase(const std::string& base) {
    std::string addr, port, proto;
    if (!applyPmsBaseToEndpoint(base, addr, port, proto))
        return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const std::string prev = serverHost_;
        serverHost_ = addr;
        serverPort_ = port;
        serverProto_ = proto;
        if (prev != addr) {
            std::string line = "companion: timeline PMS proto=" + proto + " address=" + addr +
                               " port=" + port;
            if (!prev.empty())
                line += " from " + prev;
            log(line);
        }
    }
    requestTimelinePush(true);
}

void Companion::setState(const std::string& state, int64_t timeMs, int64_t durationMs,
                         bool terminal, const CurrentFn& current) {
    std::lock_guard<std::mutex> lock(mu_);
    if (current && !current())
        return;
    if (terminalStop_ &&
        (state == "playing" || state == "paused" || state == "buffering"))
        return;
    const std::string previousState = state_;
    const bool previousTerminal = terminalStop_;
    // After stop clearMedia(), prePlayHold_ is set while wantPlay_ is false. Ignore
    // late media-thread progress so async teardown cannot re-arm fullScreenVideo.
    if (!wantPlay_ && (prePlayHold_ || userStop_) &&
        (state == "playing" || state == "paused" || state == "buffering" || state == "ended")) {
        return;
    }
    if (state == "playing" || state == "paused")
        stopDuringResolve_ = false;
    // A final decoder rejection is not a pending seek. Keep the bind/position,
    // but expose stopped until a new play or progress event starts another run.
    terminalStop_ = state == "ended" ||
                    (state == "stopped" && (terminal || terminalStop_));
    if (state == "ended" && previousState != "ended")
        endedAt_ = std::chrono::steady_clock::now();   // BUG2-RECAST idle clock
    if (terminalStop_)
        disarmHoldLocked();
    // Empty/failed session end (no frames): player reports stopped@0. Keep scrubber
    // time so a plant seek + step is not clobbered by demux short-read teardown.
    // Natural EOF uses "ended" and retains the actual final presentation time.
    if (state == "stopped" && wantPlay_) {
        if (stopDuringResolve_) {
            // The user already stopped, and this play never started. Drop the bind the
            // playMedia ACK planted so the poll is navigation, not a zombie fullScreenVideo.
            stopDuringResolve_ = false;
            pendingKey_.clear();
            pendingContainerKey_.clear();
            pendingPlayQueueId_.clear();
            pendingPlayQueueItemId_.clear();
            pendingPlayQueueVersion_.clear();
            pendingRatingKey_.clear();
            pendingToken_.clear();
            streamsKnown_ = false;
            audioStreamId_ = 0;
            subtitleStreamId_ = 0;
            wantPlay_ = false;
            state_ = "stopped";
            timeMs_ = 0;
            durationMs_ = 0;
            scrubTargetMs_ = -1;
            if (castBound_) {
                prePlayHold_ = true;
                armHoldLocked(stopHoldMs_);
            } else {
                disarmHoldLocked();
            }
            requestTimelinePush(true);
            return;
        }
#ifndef MUT_RC1036_KEEP_KEY
        // A local stop (keyboard, on-screen plex UI, browse) used to return here
        // with the playQueue still bound. Plex Web treats stopped+key as session
        // ownership, so Ready never came back and other clients stayed blocked.
        // Natural EOF already drops that bind after endedIdleMs_. Arm the same
        // clock. Web clearMedia clears the bind and wantPlay_ first, so a Web
        // stop does not reach this branch. A non-user stopped (short-read
        // teardown, including FPGA closedStop with terminal=true) still keeps
        // the key.
        if (userStop_) {
            if (timeMs < 0)
                timeMs = 0;
            if (durationMs > 0)
                durationMs_ = durationMs;
            if (durationMs_ > 0 && timeMs > durationMs_)
                timeMs = durationMs_;
            timeMs_ = timeMs;
            if (state_ != "ended")
                endedAt_ = std::chrono::steady_clock::now();
            state_ = "ended";
            terminalStop_ = true;
            scrubTargetMs_ = -1;
            disarmHoldLocked();
            requestTimelinePush(true);
            return;
        }
#endif
        state_ = state;
        if (durationMs > 0)
            durationMs_ = durationMs;
        requestTimelinePush(previousState != state_ || previousTerminal != terminalStop_);
        return;
    }
    // Scrubber bounds on incoming time before plant-hold compare.
    if (timeMs < 0)
        timeMs = 0;
    if (durationMs > 0 && timeMs > durationMs)
        timeMs = durationMs;
    else if (durationMs_ > 0 && timeMs > durationMs_)
        timeMs = durationMs_;

    // Async seek/step plant hold (P4-SCRUB / C-unit6):
    // Demux restart often reports buffering/playing@0 (or the plant pulse) before
    // real catch-up. Companion plant is scrubber source of truth until live
    // progress is near *and past* the target (or natural EOF at plant).
    //
    // Rules while scrubTargetMs_ >= 0:
    //  - buffering: pin time to the plant (companion plants via buffering@target);
    //    never release — plant call itself must not clear the hold.
    //  - playing/paused far *behind* plant (seek demux restart@0): keep plant time
    //    and transport state so the Web thumb does not rewind.
    //  - playing/paused far *ahead* of plant (stale plant 0 + viewOffset start, or
    //    first progress jump): release hold and adopt live time. The old symmetric
    //    "far = pin forever" rule froze Plex Web at 0:00 while PMS /:/timeline still
    //    advanced from the media thread (fix 0abee0b6).
    //  - playing/paused at plant pulse (near, not advanced): apply time, keep hold
    //    so late restart@0 / short-read cannot free-run after the pulse.
    //  - playing/paused advanced past plant by kScrubAdvanceMs: release + apply.
    //  - ended near plant: release + apply.
    constexpr int64_t kScrubCatchupMs = 2000;
    constexpr int64_t kScrubAdvanceMs = 400;
    if (wantPlay_ && scrubTargetMs_ >= 0 &&
        (state == "playing" || state == "paused" || state == "buffering" || state == "ended")) {
        const int64_t delta =
            timeMs > scrubTargetMs_ ? timeMs - scrubTargetMs_ : scrubTargetMs_ - timeMs;
        if (state == "buffering") {
            if (durationMs > 0)
                durationMs_ = durationMs;
            state_ = "buffering";
            // Pin thumb to plant (not demux startMs of a superseded seek).
            timeMs_ = scrubTargetMs_;
            wantPlay_ = true;
            requestTimelinePush(previousState != state_ || previousTerminal != terminalStop_);
            return;
        }
        if (delta > kScrubCatchupMs) {
            // Only suppress rewinds. Demux well ahead of plant is live truth.
            if (timeMs + kScrubCatchupMs < scrubTargetMs_) {
                if (durationMs > 0)
                    durationMs_ = durationMs;
                if (state == "playing" || state == "paused")
                    state_ = state;
                wantPlay_ = true;
                requestTimelinePush(previousState != state_);
                return;
            }
            scrubTargetMs_ = -1;
            // fall through: adopt live timeMs
        } else if (state == "ended" ||
                   ((state == "playing" || state == "paused") &&
                    timeMs >= scrubTargetMs_ + kScrubAdvanceMs)) {
            // Near plant: release only after demux advances past plant, or on ended.
            scrubTargetMs_ = -1;
        }
        // else playing@plant pulse: fall through apply time, keep hold
    }


    state_ = state;
    if (durationMs > 0)
        durationMs_ = durationMs;
    // Scrubber bounds: never report negative time or time past known duration.
    if (durationMs_ > 0 && timeMs > durationMs_)
        timeMs = durationMs_;
    timeMs_ = timeMs;
    // Keep wantPlay_ latched after playMedia until clearMedia()/stop.
    // Player progress "stopped" (EOF) must not drop scrubber bind fields.
    if (!userStop_ && (state == "playing" || state == "paused" || state == "buffering"))
        wantPlay_ = true;
    // Epoch always bumps, so wait=1 wakes on time ticks. immediate stays state-change only:
    // every FPGA ACK as immediate would POST /:/timeline at frame rate.
    requestTimelinePush(previousState != state_ || previousTerminal != terminalStop_);
}

void Companion::noteStopDuringResolve() {
    std::lock_guard<std::mutex> lock(mu_);
    stopDuringResolve_ = true;
}

bool Companion::bindMedia(const PlayRequest& req, int64_t durationMs) {
    std::lock_guard<std::mutex> lock(mu_);
    // Drop late async playMedia (resolve/network) that finishes after stop/clearMedia
    // so scrubber cannot re-arm fullScreenVideo without a fresh cast command.
    if (!wantPlay_) {
        log("bindMedia ignored — session stopped (stale playMedia)");
        return false;
    }
    // Newer playMedia/stagePlay already planted a different key — stale resolve.
    if (!pendingKey_.empty() && !req.key.empty() && pendingKey_ != req.key) {
        log("bindMedia ignored — key mismatch (stale) pending=" + pendingKey_ + " got=" +
            req.key);
        return false;
    }
    userStop_ = false;
    pendingKey_ = req.key;
    idleKey_.clear();   // a new bind replaces the stopped item identity
    pendingContainerKey_ = req.containerKey;
    pendingPlayQueueId_ = req.playQueueId;
    pendingPlayQueueItemId_ = req.playQueueItemId;
    pendingPlayQueueVersion_ = req.playQueueVersion.empty() ? "1" : req.playQueueVersion;
    if (pendingContainerKey_.empty() && !pendingPlayQueueId_.empty())
        pendingContainerKey_ = "/playQueues/" + pendingPlayQueueId_ + "?own=1";
    pendingRatingKey_ = req.ratingKey;
    pendingToken_ = req.token;
    serverMachineId_ = req.serverMachineId;
    if (!req.peerAddress.empty())
        serverPeer_ = req.peerAddress;
    serverTargetRejected_ = req.targetRejected;
    if (req.targetRejected)
        clearRefusedServerLocked();
    else if (!req.address.empty())
        bindTimelinePmsLocked(req.address, req.port, req.protocol);
    else if (!req.protocol.empty() || !req.port.empty()) {
        if (!req.protocol.empty())
            serverProto_ = req.protocol;
        if (!req.port.empty())
            serverPort_ = req.port;
    }
    // Always take resolve duration (0 = unknown / local file without probe)
    durationMs_ = durationMs > 0 ? durationMs : 0;
    // If playMedia planted a huge offset before duration was known, clamp now.
    if (timeMs_ < 0)
        timeMs_ = 0;
    if (durationMs_ > 0 && timeMs_ > durationMs_)
        timeMs_ = durationMs_;
    // Keep plant hold across resolve: demux may still report playing@0 after bind.
    // Only playing/paused near-target in setState releases scrubTargetMs_.
    if (scrubTargetMs_ >= 0) {
        if (durationMs_ > 0 && scrubTargetMs_ > durationMs_)
            scrubTargetMs_ = durationMs_;
        // Thumb follows clamped plant (continue-watching past shorter stale duration).
        timeMs_ = scrubTargetMs_;
    }
    wantPlay_ = true;
    prePlayHold_ = false;
    disarmHoldLocked();
    requestTimelinePush(true);
    return true;
}

void Companion::setActiveHeight(int activeHeight, int picHeight) {
    std::lock_guard<std::mutex> lock(mu_);
    activeHeight_ = activeHeight;
    picHeight_ = picHeight;
}

void Companion::setRasterMode(const std::string& mode) {
    std::lock_guard<std::mutex> lock(mu_);
    rasterMode_ = mode;
    if (const RasterHeightRow* row = rasterHeightFor(mode)) {
        activeHeight_ = row->active_height;
        picHeight_ = row->pic_height;
    }
}

void Companion::seedPlaybackPosition(int64_t timeMs, int64_t durationMs, const CurrentFn& current) {
    std::lock_guard<std::mutex> lock(mu_);
    if (current && !current())
        return;
    if (timeMs < 0)
        timeMs = 0;
    if (durationMs > 0) {
        durationMs_ = durationMs;
        if (timeMs > durationMs_)
            timeMs = durationMs_;
    } else if (durationMs_ > 0 && timeMs > durationMs_) {
        timeMs = durationMs_;
    }
    timeMs_ = timeMs;
    // Re-base hold on the real start. Without this, playMedia plant@0 + PMS
    // viewOffset demux start left Web polls frozen at 0 while media ran ahead.
    scrubTargetMs_ = timeMs;
    if (wantPlay_ && (state_ == "stopped" || state_.empty())) {
        state_ = "buffering";
        terminalStop_ = false;
    }
    requestTimelinePush(true);
}

bool Companion::stagePlay(const PlayRequest& req, const CurrentFn& current) {
    // Plant scrubber identity for skipNext/auto-next before async resolve so
    // bindMedia key-match accepts this title and Web sees the advance early.
    std::lock_guard<std::mutex> lock(mu_);
    if (current && !current())
        return false;
    userStop_ = false;
    wantPlay_ = true;
    prePlayHold_ = false;
    disarmHoldLocked();
    castBound_ = true;
    state_ = "buffering";
    terminalStop_ = false;
    durationMs_ = 0;
    int64_t off = req.offsetMs < 0 ? 0 : req.offsetMs;
    if (!req.offsetPresent && lastGoodPositionMs_ > 0 &&
        !lastGoodKey_.empty() && (req.key.empty() || req.key == lastGoodKey_))
        off = lastGoodPositionMs_;
    timeMs_ = off;
    scrubTargetMs_ = timeMs_;
    lastGoodPositionMs_ = 0;
    lastGoodKey_.clear();
    pendingKey_ = req.key;
    idleKey_.clear();   // a new bind replaces the stopped item identity
    pendingToken_ = req.token;
    pendingGeneration_ = req.dispatchGeneration;
    pendingContainerKey_ = req.containerKey;
    const std::string prevQueueId = pendingPlayQueueId_;
    const std::string prevQueueVersion = pendingPlayQueueVersion_;
    pendingPlayQueueId_ = req.playQueueId;
    pendingPlayQueueItemId_ = req.playQueueItemId;
    pendingPlayQueueVersion_ = playQueueVersionAfterReplace(
        prevQueueId, prevQueueVersion, req.playQueueId, req.playQueueVersion);
    if (pendingContainerKey_.empty() && !pendingPlayQueueId_.empty())
        pendingContainerKey_ = "/playQueues/" + pendingPlayQueueId_ + "?own=1";
    if (!req.ratingKey.empty())
        pendingRatingKey_ = req.ratingKey;
    playDecision_.clear();
    serverTargetRejected_ = req.targetRejected;
    if (req.targetRejected) {
        clearRefusedServerLocked();
    } else if (!req.address.empty()) {
        bindTimelinePmsLocked(req.address, req.port, req.protocol);
    } else {
        if (!req.port.empty())
            serverPort_ = req.port;
        if (!req.protocol.empty())
            serverProto_ = req.protocol;
    }
    if (!req.serverMachineId.empty())
        serverMachineId_ = req.serverMachineId;
    if (!req.peerAddress.empty())
        serverPeer_ = req.peerAddress;
    requestTimelinePush(true);
    return true;
}

// #355 B1: a refused playMedia target must not leave the previous cast's host bound to the new token.
void Companion::clearRefusedServerLocked() {
#ifndef MUT_355_COMPANION_KEEPS_HOST
    serverHost_.clear();
    serverPort_.clear();
    serverProto_.clear();
#endif
}

TransportRequest Companion::transportRequestLocked(TransportCommand command) {
    TransportRequest request = onTransportQueued_ ? onTransportQueued_(command)
                                                 : TransportRequest{};
    request.positionMs = timeMs_;
    request.originGeneration = pendingGeneration_;
    auto& media = request.media;
    media.key = pendingKey_;
    media.ratingKey = pendingRatingKey_;
    media.containerKey = pendingContainerKey_;
    media.playQueueId = pendingPlayQueueId_;
    media.playQueueItemId = pendingPlayQueueItemId_;
    media.playQueueVersion = pendingPlayQueueVersion_;
    media.address = serverHost_;
    media.protocol = serverProto_;
    media.port = serverPort_;
    media.serverMachineId = serverMachineId_;
    media.peerAddress = serverPeer_;
    media.token = pendingToken_;
    media.targetRejected = serverTargetRejected_;
    media.offsetMs = timeMs_;
    media.offsetPresent = true;
    media.dispatchGeneration = request.generation;
    return request;
}

bool Companion::acceptSeekLocked(int64_t ms, TransportRequest& request) {
    ms = std::max<int64_t>(0, ms);
    if (durationMs_ > 0)
        ms = std::min(ms, durationMs_);
    if (!wantPlay_ || ms == timeMs_)
        return false;
    request = transportRequestLocked(TransportCommand::Seek);
    request.media.offsetMs = ms;
    pendingGeneration_ = request.generation;
    scrubTargetMs_ = timeMs_ = ms;
    state_ = "buffering";
    terminalStop_ = false;
    return true;
}

void Companion::setLetterboxBars(int barTop, int barBot, int barLeft, int barRight) {
    std::lock_guard<std::mutex> lock(mu_);
    barTop_ = barTop < 0 ? 0 : barTop;
    barBot_ = barBot < 0 ? 0 : barBot;
    barLeft_ = barLeft < 0 ? 0 : barLeft;
    barRight_ = barRight < 0 ? 0 : barRight;
}

bool Companion::seekTo(int64_t ms, const CurrentFn& current) {
    TransportRequest request;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if ((current && !current()) || !acceptSeekLocked(ms, request))
            return false;
    }
    requestTimelinePush(true);
    if (onSeek_)
        onSeek_(request);
    return true;
}

bool Companion::acceptPauseResumeLocked(bool pause, TransportRequest& request) {
    if (!wantPlay_ || terminalStop_)
        return false;
    request = transportRequestLocked(pause ? TransportCommand::Pause : TransportCommand::Resume);
    state_ = pause ? "paused" : "playing";
    return true;
}

TransportRequest Companion::clearMedia() {
    {
        std::lock_guard<std::mutex> subscribers(subscriberMu_);
        castOwnerId_.clear();
        castOwnerHost_.clear();
        castOwnerSet_ = false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    userStop_ = true;
    auto request = transportRequestLocked(TransportCommand::Stop);
    // A stop in the finished zone (>= ~90 % or last 5 s) is not a resume point: PMS calls the item watched, so the next
    // cast without an offset must start at 0, not at this stale position.
    if (inFinishedZone(timeMs_, durationMs_)) {
        lastGoodKey_.clear();
        lastGoodPositionMs_ = 0;
    } else {
        lastGoodKey_ = pendingKey_;
        lastGoodPositionMs_ = timeMs_ < 0 ? 0 : timeMs_;
    }
    if (idleKeyMs_ > 0 && !pendingKey_.empty()) {
        idleKey_ = pendingKey_;
        idleRatingKey_ = pendingRatingKey_;
        idleContainer_ = containerOf(pendingContainerKey_, pendingPlayQueueId_);
        idleKeyUntil_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(idleKeyMs_);
    }
    // Drop media binding so polls are a clean idle (no key/container).
    // Web: video state=stopped WITH key still idles the cast player and freezes scrubber.
    pendingKey_.clear();
    pendingContainerKey_.clear();
    pendingPlayQueueId_.clear();
    pendingPlayQueueItemId_.clear();
    pendingPlayQueueVersion_.clear();
    pendingRatingKey_.clear();
    pendingToken_.clear();
    pendingGeneration_ = 0;
    streamsKnown_ = false;
    audioStreamId_ = 0;
    subtitleStreamId_ = 0;
    playDecision_.clear();
    rasterMode_.clear();
    activeHeight_ = 0;
    picHeight_ = 0;
    wantPlay_ = false;
    state_ = "stopped";
    // A browser Stop after terminal EOF must not reopen the buffering hold.
    const int64_t knownTimeMs = timeMs_;
    const int64_t knownDurMs = durationMs_;
    timeMs_ = 0;
    durationMs_ = 0;
    scrubTargetMs_ = -1;
    resumeAfterRespawn_ = 0;
    barTop_ = 0;
    barBot_ = 0;
    barLeft_ = 0;
    barRight_ = 0;
    // Post-Stop hold: while cast-bound, Web often reopens Resume without a fresh
    // mirror and pure stopped idles the dialog, so keep buffering@navigation, but
    // only for MPX_COMPANION_STOP_HOLD_MS; then the wire reports stopped.
    if (castBound_) {
        prePlayHold_ = true;
        armHoldLocked(stopHoldMs_);
        holdTimeMs_ = knownTimeMs;
        holdDurMs_ = knownDurMs;
    } else {
        disarmHoldLocked();
    }
    requestTimelinePush(true);
    return request;
}

int Companion::holdMsFromEnv(const char* name, int fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v)
        return fallback;
    char* end = nullptr;
    errno = 0;
    const long ms = std::strtol(v, &end, 10);
    if (errno != 0 || end == v || *end != '\0' || ms < 0)
        return fallback;
    return static_cast<int>(std::min<long>(ms, 120000));
}

void Companion::armHoldLocked(int ms) {
    if (ms <= 0) {
        disarmHoldLocked();
        return;
    }
    holdUntil_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    holdTimeMs_ = 0;
    holdDurMs_ = 0;
    {
        std::lock_guard<std::mutex> lock(timelinePushMu_);
        holdExpiryPending_ = true;
        holdExpiry_ = holdUntil_;
    }
    timelinePushCv_.notify_one();
}

void Companion::disarmHoldLocked() {
    holdUntil_ = {};
    holdTimeMs_ = 0;
    holdDurMs_ = 0;
    std::lock_guard<std::mutex> lock(timelinePushMu_);
    holdExpiryPending_ = false;
}

void Companion::requestTimelinePush(bool immediate) {
    timelineGen_.fetch_add(1);   // releases a held wait=1 poll (companion_poll_hold.hpp)
    {
        std::lock_guard<std::mutex> lock(timelinePushMu_);
        timelinePushPending_ = true;
        timelinePushImmediate_ = timelinePushImmediate_ || immediate;
    }
    timelinePushCv_.notify_one();
}

void Companion::subscribeTimeline(const std::string& id, const std::string& host,
                                  const std::string& protocol, uint16_t port,
                                  const std::string& commandId, const std::string& peer) {
    TimelineSubscriber subscriber;
    subscriber.id = id.empty() ? host + ":" + std::to_string(port) : id;
    subscriber.host = host;
    // The socket peer is the identity. A caller that has no separate peer
    // (unit tests) is the callback host. address= never overwrites a real peer.
    subscriber.peer = peer.empty() ? host : peer;
    subscriber.protocol = protocol;
    subscriber.port = port;
    subscriber.commandId = commandId.empty() ? "0" : commandId;

    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(subscriberMu_);
        auto it = std::find_if(
            subscribers_.begin(), subscribers_.end(),
            [&](const TimelineSubscriber& current) { return current.id == subscriber.id; });
        if (it != subscribers_.end()) {
            // A copied client id from another socket must not kick the owner off
            // the push list, even if address= names the owner's IP. The same
            // socket peer may refresh port and commandID.
#ifdef MUT_TIMELINE_SUBSCRIBE_ADDRESS
            const bool sameSubscriber = it->host == subscriber.host;
#else
            const bool sameSubscriber = !subscriber.peer.empty() && it->peer == subscriber.peer;
#endif
            if (!sameSubscriber) {
                log("timeline: subscriber id kept; replacement from peer " + subscriber.peer +
                    " refused (registered peer " + it->peer + ")");
            } else {
                *it = subscriber;
            }
        } else {
            constexpr size_t kMaxSubscribers = 8;
            if (subscribers_.size() >= kMaxSubscribers)
                subscribers_.erase(subscribers_.begin());
            subscribers_.push_back(std::move(subscriber));
        }
        count = subscribers_.size();
    }
    log("timeline: subscriber registered peer=" + host + ":" + std::to_string(port) +
        " count=" + std::to_string(count));
    requestTimelinePush(true);
}

void Companion::updateTimelineCommand(const std::string& id, const std::string& host,
                                      const std::string& commandId) {
    if (commandId.empty())
        return;
    std::lock_guard<std::mutex> lock(subscriberMu_);
    auto controller = std::find_if(
        controllerCommands_.begin(), controllerCommands_.end(),
        [&](const ControllerCommand& current) {
            return !id.empty() ? current.id == id
                               : current.id.empty() && current.host == host;
        });
    // The LATEST commandID this controller sent, never a high-water mark: Plex Web restarts its
    // counter at 1 after a reload / new session, and a max-seen echo (629 to a page at 2) is a
    // stale reply ( lab glass 1011 TNG run, 132/132 polls). A request that carries a commandID is
    // answered with exactly that commandID.
    if (controller != controllerCommands_.end()) {
        controller->commandId = commandId;
    } else {
        constexpr size_t kMaxControllers = 16;
        if (controllerCommands_.size() >= kMaxControllers)
            controllerCommands_.erase(controllerCommands_.begin());
        controllerCommands_.push_back(ControllerCommand{id, host, commandId});
    }
    for (auto& subscriber : subscribers_) {
        if ((!id.empty() && subscriber.id == id) || (id.empty() && subscriber.host == host))
            subscriber.commandId = commandId;
    }
}

std::string Companion::timelineCommandFor(const std::string& id, const std::string& host,
                                          const std::string& commandId) {
    updateTimelineCommand(id, host, commandId);
    std::lock_guard<std::mutex> lock(subscriberMu_);
    auto find = [&](const std::string& cid, const std::string& chost) {
        return std::find_if(controllerCommands_.begin(), controllerCommands_.end(),
                            [&](const ControllerCommand& current) {
                                return !cid.empty() ? current.id == cid
                                                    : current.id.empty() && current.host == chost;
                            });
    };
    const auto controller = find(id, host);
    std::string result = controller == controllerCommands_.end() ? "0" : controller->commandId;
    // No commandID on this request (e.g. a relay that strips the browser's id and
    // commandID): never answer below the active cast owner's commandID, or Plex Web
    // (applies a timeline only when commandID >= its counter) drops every reply.
    if (commandId.empty() && castOwnerSet_) {
        const auto owner = find(castOwnerId_, castOwnerHost_);
        if (owner != controllerCommands_.end() && commandIsNewerOrEqual(owner->commandId, result))
            result = owner->commandId;
    }
    return result;
}

void Companion::setCastOwner(const std::string& id, const std::string& host) {
    std::lock_guard<std::mutex> lock(subscriberMu_);
    castOwnerSet_ = true;
    castOwnerId_ = id;
    castOwnerHost_ = host;
}

void Companion::noteCastOwner(const std::string& id, const std::string& host,
                              const std::string& suppliedToken) {
    bool sameId = false;
    bool verifiedOwner = false;
    {
        std::lock_guard<std::mutex> subscribers(subscriberMu_);
        sameId = castOwnerSet_ && !id.empty() && id == castOwnerId_;
        verifiedOwner = sameId && !host.empty() && host == castOwnerHost_;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!suppliedToken.empty())
            pendingToken_ = suppliedToken;
        else {
            // No token=: keep the stored credential only for the verified owner.
            // Same client id from another peer is not that owner.
            bool keep = verifiedOwner;
#ifdef MUT_TIMELINE_REUSE_ON_ID
            keep = sameId;
#endif
#ifdef MUT_TIMELINE_KEEP_OLD_TOKEN
            keep = true;
#endif
            if (!keep)
                pendingToken_.clear();
        }
    }
    setCastOwner(id, host);
}

std::string Companion::tokenRecipientForPush(const TimelineSubscriber& subscriber) const {
#ifdef MUT_TIMELINE_PUSH_HOST_ONLY
    std::lock_guard<std::mutex> subscribers(subscriberMu_);
    if (!castOwnerId_.empty() && !castOwnerHost_.empty() && subscriber.host == castOwnerHost_)
        return castOwnerId_;
#endif
    return subscriber.id;
}

std::string Companion::tokenPeerForPush(const TimelineSubscriber& subscriber) const {
    if (subscriber.peer.empty() || subscriber.host != subscriber.peer)
        return {};
    return subscriber.host;
}

std::string Companion::pushCommandFor(const TimelineSubscriber& subscriber) {
    // Only a subscriber that never supplied a commandID (still "0", e.g. a relay's
    // subscription) gets the owner floor; a controller's own commandID is unchanged.
    std::lock_guard<std::mutex> lock(subscriberMu_);
    const std::string own = subscriber.commandId.empty() ? "0" : subscriber.commandId;
    if (own != "0" || !castOwnerSet_)
        return own;
    const auto owner = std::find_if(
        controllerCommands_.begin(), controllerCommands_.end(),
        [&](const ControllerCommand& current) {
            return !castOwnerId_.empty() ? current.id == castOwnerId_
                                         : current.id.empty() && current.host == castOwnerHost_;
        });
    return owner == controllerCommands_.end() ? own : owner->commandId;
}

bool Companion::unsubscribeTimeline(const std::string& id, const std::string& host) {
    size_t removed = 0;
    size_t remaining = 0;
    {
        std::lock_guard<std::mutex> lock(subscriberMu_);
        const auto before = subscribers_.size();
        subscribers_.erase(
            std::remove_if(subscribers_.begin(), subscribers_.end(),
                           [&](const TimelineSubscriber& subscriber) {
#ifdef MUT_TIMELINE_UNSUB_ID_ONLY
                               return !id.empty() ? subscriber.id == id
                                                  : subscriber.host == host;
#else
                               // Same client id and the socket peer that subscribed.
                               // address= is the callback host and does not authorize a drop.
                               return !id.empty() && subscriber.id == id &&
                                      !host.empty() && subscriber.peer == host;
#endif
                           }),
            subscribers_.end());
        removed = before - subscribers_.size();
        remaining = subscribers_.size();
        // Subscription ended: forget that controller's last commandID too, so a later session
        // (or a commandID-less request from it) never echoes the old session's value.
        controllerCommands_.erase(
            std::remove_if(controllerCommands_.begin(), controllerCommands_.end(),
                           [&](const ControllerCommand& current) {
#ifdef MUT_TIMELINE_UNSUB_ID_ONLY
                               return !id.empty() ? current.id == id
                                                  : current.id.empty() && current.host == host;
#else
                               return !id.empty() && current.id == id &&
                                      !host.empty() && current.host == host;
#endif
                           }),
            controllerCommands_.end());
    }
    if (removed > 0) {
        log("timeline: subscriber removed peer=" + host +
            " remaining=" + std::to_string(remaining));
    }
    return remaining == 0;
}

bool Companion::postTimeline(const TimelineSubscriber& subscriber,
                             const std::string& xml) const {
    if (subscriber.protocol != "http")
        return false;

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(subscriber.port);
    if (inet_pton(AF_INET, subscriber.host.c_str(), &peer.sin_addr) != 1)
        return false;

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    setCloexec(fd);

    const int oldFlags = fcntl(fd, F_GETFL, 0);
    if (oldFlags >= 0)
        fcntl(fd, F_SETFL, oldFlags | O_NONBLOCK);
    int rc = connect(fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
    if (rc != 0 && errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        timeval timeout{1, 0};
        rc = select(fd + 1, nullptr, &wfds, nullptr, &timeout);
        if (rc > 0) {
            int socketError = 0;
            socklen_t errorLen = sizeof(socketError);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &errorLen) != 0 ||
                socketError != 0) {
                rc = -1;
            } else {
                rc = 0;
            }
        } else {
            rc = -1;
        }
    }
    if (rc != 0) {
        close(fd);
        return false;
    }
    if (oldFlags >= 0)
        fcntl(fd, F_SETFL, oldFlags);

    timeval ioTimeout{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &ioTimeout, sizeof(ioTimeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &ioTimeout, sizeof(ioTimeout));

    std::ostringstream request;
    std::string token;
    std::string ownerId;
    std::string ownerHost;
    {
        std::lock_guard<std::mutex> subscribers(subscriberMu_);
        ownerId = castOwnerId_;
        ownerHost = castOwnerHost_;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        // The callback host has to be this row's own subscribing peer. address=
        // must not aim the owner's token at another host.
        const bool callbackIsPeer =
            !subscriber.peer.empty() && subscriber.host == subscriber.peer;
        bool hostOk = !ownerHost.empty() && subscriber.host == ownerHost && callbackIsPeer;
#ifdef MUT_TIMELINE_TOKEN_IGNORE_CALLBACK_HOST
        hostOk = true;
#endif
        bool idOk = !ownerId.empty() && subscriber.id == ownerId;
#ifdef MUT_TIMELINE_PUSH_HOST_ONLY
        idOk = !ownerId.empty() && hostOk;
#endif
        bool headerForSubscriber = idOk && hostOk;
#ifdef MUT_TIMELINE_TOKEN_ANY_CLIENT
        headerForSubscriber = true;
#endif
        if (headerForSubscriber)
            token = pendingToken_;
#ifdef MUT_TIMELINE_OMIT_CAST_TOKEN
        token.clear();
#endif
    }
    std::string cmd = subscriber.commandId.empty() ? "0" : subscriber.commandId;
#ifdef MUT_PB_CMD_ZERO
    cmd = "0";
#endif
    request << "POST /:/timeline?commandID=" << cmd << " HTTP/1.1\r\n"
            << "Host: " << subscriber.host << ":" << subscriber.port << "\r\n"
            << "Content-Type: application/xml\r\n"
            << "Content-Length: " << xml.size() << "\r\n"
            << "X-Plex-Client-Identifier: " << machineId_ << "\r\n";
    if (!token.empty())
        request << "X-Plex-Token: " << token << "\r\n";
    request << "Connection: close\r\n\r\n"
            << xml;
    if (!sendAll(fd, request.str())) {
        close(fd);
        return false;
    }

    std::string response;
    const auto readResult = recvHttpHeaders(fd, response, 4096, 1000);
    close(fd);
    if (readResult != HttpHeaderRead::Ok)
        return false;
    const auto lineEnd = response.find("\r\n");
    const auto space = response.find(' ');
    if (space == std::string::npos ||
        (lineEnd != std::string::npos && space >= lineEnd) ||
        response.size() - space < 4)
        return false;
    const int status = std::atoi(response.c_str() + space + 1);
    return status >= 200 && status < 300;
}

bool Companion::keepaliveWanted() {
#ifdef MUT_PB_NO_KEEPALIVE
    return false;
#endif
#ifdef MUT_PB_KEEPALIVE_ALWAYS
    return true;   // mutant: keepalive even with no subscriber / no play
#endif
    {
        std::lock_guard<std::mutex> lock(subscriberMu_);
        if (subscribers_.empty())
            return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (!wantPlay_ || terminalStop_)
        return false;
    return state_ == "playing" || state_ == "paused" || state_ == "buffering";
}

void Companion::timelinePushLoop() {
    using Clock = std::chrono::steady_clock;
    auto nextPeriodic = Clock::now();
    auto lastPushAt = Clock::now();
    while (running_.load()) {
        // Evaluated before timelinePushMu_ is taken: setState() holds mu_ while it requests a push.
        const bool keepalive = keepaliveWanted();
        {
            std::unique_lock<std::mutex> lock(timelinePushMu_);
            for (;;) {
                if (!running_.load())
                    return;
                const auto now = Clock::now();
                // One push when the wire hold expires, so subscribers see stopped.
                if (holdExpiryPending_ && now >= holdExpiry_) {
                    holdExpiryPending_ = false;
                    break;
                }
                if (timelinePushPending_ && (timelinePushImmediate_ || now >= nextPeriodic))
                    break;
                const auto keepaliveDue = lastPushAt + kSubscriberKeepalive;
                if (keepalive && now >= keepaliveDue)
                    break;   // nothing changed for kSubscriberKeepalive: re-send the current timeline
                auto wake = Clock::time_point::max();
                if (timelinePushPending_)
                    wake = nextPeriodic;
                if (keepalive && keepaliveDue < wake)
                    wake = keepaliveDue;
                if (holdExpiryPending_ && holdExpiry_ < wake)
                    wake = holdExpiry_;
                if (wake == Clock::time_point::max())
                    timelinePushCv_.wait(lock);
                else
                    timelinePushCv_.wait_until(lock, wake);
            }
            timelinePushPending_ = false;
            timelinePushImmediate_ = false;
        }

        std::vector<TimelineSubscriber> subscribers;
        {
            std::lock_guard<std::mutex> lock(subscriberMu_);
            subscribers = subscribers_;
        }
        for (const auto& subscriber : subscribers) {
#ifdef MUT_TIMELINE_PUSH_LOOP_OWNER_ID
            std::string tokenId;
            {
                std::lock_guard<std::mutex> subscribers(subscriberMu_);
                tokenId = castOwnerId_;
            }
#else
            const std::string tokenId = tokenRecipientForPush(subscriber);
#endif
            const bool ok = postTimeline(
                subscriber, timelineXml(pushCommandFor(subscriber), tokenId,
                                        tokenPeerForPush(subscriber)));
            bool removed = false;
            {
                std::lock_guard<std::mutex> lock(subscriberMu_);
                auto it = std::find_if(
                    subscribers_.begin(), subscribers_.end(),
                    [&](const TimelineSubscriber& current) {
                        return current.id == subscriber.id && current.host == subscriber.host &&
                               current.port == subscriber.port;
                    });
                if (it == subscribers_.end())
                    continue;
                if (ok) {
                    it->failures = 0;
                } else if (++it->failures >= 3) {
                    subscribers_.erase(it);
                    removed = true;
                }
            }
            if (removed) {
                log("timeline: subscriber dropped after callback failures peer=" +
                    subscriber.host + ":" + std::to_string(subscriber.port));
            }
        }
        nextPeriodic = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        lastPushAt = std::chrono::steady_clock::now();
    }
}

bool Companion::openHttpListen() {
    for (int attempt = 1; attempt <= 8; ++attempt) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            log("HTTP: socket failed");
            return false;
        }
        setReuse(fd);
        setCloexec(fd);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);
        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && listen(fd, 16) == 0) {
            httpListenFd_ = fd;
            httpReady_.store(true);
            log("HTTP: companion on :" + std::to_string(port_) + " ip≈" + lanIp());
            return true;
        }
        log("HTTP: bind :" + std::to_string(port_) + " failed attempt=" + std::to_string(attempt));
        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

void Companion::setPlexTvEnabled(bool on) {
    std::lock_guard<std::mutex> lock(mu_);
    plexTvEnabled_ = on;
}

void Companion::setPlexTvToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(mu_);
    plexTvToken_ = token;
}

void Companion::setPlexTvLinkPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(mu_);
    plexTvLinkPath_ = path;
}

void Companion::setPlexTvPersist(std::function<bool(const std::string&)> fn) {
    std::lock_guard<std::mutex> lock(mu_);
    plexTvPersist_ = std::move(fn);
}

std::string Companion::plexTvCode() const {
    std::lock_guard<std::mutex> lock(mu_);
    return plexTvCode_;
}

void Companion::publishPlexTvCode(const std::string& code) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mu_);
        plexTvCode_ = code;
        path = plexTvLinkPath_;
    }
    if (!path.empty())
        writePlexTvLinkFile(path, isPlexTvLinkMarker(code) ? std::string() : code);
}

void Companion::plexTvLoop() {
    std::string ident;
    std::function<bool(const std::string&)> persist;
    {
        std::lock_guard<std::mutex> lock(mu_);
        ident = machineId_;
        persist = plexTvPersist_;
    }
    const auto post = plexui::makePlexuiHttpPost();
    const auto get = plexui::makePlexuiHeaderGet();
    runPlexTvLinkUntilSigned(
        post, get, ident, [this] { return running_.load(); },
        [this](const std::string& code) { publishPlexTvCode(code); },
        persist,
        [this](const std::string& line) { log(line); });
}

bool Companion::start() {
    if (running_.exchange(true))
        return true;
    log("companion: wire hold stop_ms=" + std::to_string(stopHoldMs_) +
        " mirror_ms=" + std::to_string(mirrorHoldMs_));
    httpReady_.store(false);
    if (!openHttpListen()) {
        running_.store(false);
        log("companion: HTTP listen failed — not advertising GDM");
        return false;
    }
    gdmThr_ = std::thread([this] {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "mpx-gdm");
#endif
        gdmLoop();
    });
    httpThr_ = std::thread([this] {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "mpx-http");
#endif
        httpLoop();
    });
    timelinePushThr_ = std::thread([this] {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "mpx-comp-push");
#endif
        timelinePushLoop();
    });
    bool armLink = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        armLink = plexTvEnabled_ && plexTvToken_.empty();
    }
    if (armLink) {
        plexTvThr_ = std::thread([this] {
#if defined(__linux__)
            pthread_setname_np(pthread_self(), "mpx-plextv");
#endif
            plexTvLoop();
        });
        log("companion: plex.tv link armed");
    }
    log("companion: GDM + HTTP :" + std::to_string(port_) + " name=" + name_);
    return true;
}

void Companion::stop() {
    {
        std::lock_guard<std::mutex> subscribers(subscriberMu_);
        castOwnerId_.clear();
        castOwnerHost_.clear();
        castOwnerSet_ = false;
    }
    if (!running_.exchange(false))
        return;
    httpReady_.store(false);
    timelinePushCv_.notify_all();
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port_);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        close(fd);
    }
    if (gdmThr_.joinable())
        gdmThr_.join();
    if (httpThr_.joinable())
        httpThr_.join();
    if (timelinePushThr_.joinable())
        timelinePushThr_.join();
    if (plexTvThr_.joinable())
        plexTvThr_.join();
    log("companion: stopped");
}

void Companion::gdmLoop() {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        log("GDM: socket failed");
        return;
    }
    setReuse(fd);
    setCloexec(fd);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(32412);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        log("GDM: bind 32412 failed — broadcast-only advertise");
    else
        log("GDM: listening UDP 32412");
    // Do not answer M-SEARCH or broadcast until HTTP :port is listening.
    // Otherwise Plex Web lists MiSTerPlex and the picker vanishes (bind :3005 failed).

    auto lastAdv = std::chrono::steady_clock::now();
    auto sendFrom = [](const misterplex::GdmDest& d, const std::string& payload) {
        int s = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0)
            return;
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
        sockaddr_in src{};
        src.sin_family = AF_INET;
        src.sin_port = 0;
        if (inet_pton(AF_INET, d.src.c_str(), &src.sin_addr) != 1 ||
            bind(s, reinterpret_cast<sockaddr*>(&src), sizeof(src)) != 0) {
            close(s);
            return;
        }
        if (d.hello) {
            in_addr ifaddr{};
            if (inet_pton(AF_INET, d.src.c_str(), &ifaddr) == 1)
                setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr));
        }
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(d.port);
        if (inet_pton(AF_INET, d.dst.c_str(), &dst.sin_addr) != 1) {
            close(s);
            return;
        }
        sendto(s, payload.data(), payload.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        close(s);
    };
    auto collectIfaces = []() {
        std::vector<misterplex::GdmIface> ifs;
        ifaddrs* ifaddr = nullptr;
        if (getifaddrs(&ifaddr) != 0)
            return ifs;
        for (ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                continue;
            misterplex::GdmIface row;
            row.up = (ifa->ifa_flags & IFF_UP) != 0;
            row.loopback = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
            char ip[64] = {};
            auto* sin = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
            if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)))
                continue;
            row.ip = ip;
            if ((ifa->ifa_flags & IFF_BROADCAST) && ifa->ifa_broadaddr &&
                ifa->ifa_broadaddr->sa_family == AF_INET) {
                char b[64] = {};
                auto* bsin = reinterpret_cast<sockaddr_in*>(ifa->ifa_broadaddr);
                if (inet_ntop(AF_INET, &bsin->sin_addr, b, sizeof(b)))
                    row.broadcast = b;
            }
            ifs.push_back(std::move(row));
        }
        freeifaddrs(ifaddr);
        return ifs;
    };
    while (running_.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{0, 200000};
        int r = select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (r < 0) {
            // Avoid tight spin on repeated select errors.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if (r > 0 && FD_ISSET(fd, &rfds)) {
            char buf[2048];
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (n > 0) {
                buf[n] = 0;
                // M-SEARCH-only (gdm_filter.hpp). Bare "plex" self-advertise loop
                // was the Sweep 114 108% core spin (mpx-gdm / unnamed tid).
                char peerIp[INET_ADDRSTRLEN] = {};
                ::inet_ntop(AF_INET, &peer.sin_addr, peerIp, sizeof(peerIp));
                // F2190: GDM is L2. Do not reply to a WAN unicast peer; WAN PMS
                // player list stays empty without a remote client registration.
                if (misterplex::gdmMayReplyToPeer(httpReady_.load(), buf, static_cast<size_t>(n),
                                                 peerIp)) {
                    auto payload = gdmPayload();
                    sendto(fd, payload.data(), payload.size(), 0, reinterpret_cast<sockaddr*>(&peer),
                           plen);
                }
            }
        }
        auto now = std::chrono::steady_clock::now();
        if (misterplex::gdmMayAdvertise(httpReady_.load()) &&
            now - lastAdv > std::chrono::seconds(5)) {
            lastAdv = now;
            const auto dests = misterplex::gdmAdvertiseDests(collectIfaces());
            if (dests.empty()) {
                int on = 1;
                setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
                sockaddr_in bcast{};
                bcast.sin_family = AF_INET;
                bcast.sin_port = htons(misterplex::kGdmListenPort);
                bcast.sin_addr.s_addr = INADDR_BROADCAST;
                auto payload = gdmPayload();
                sendto(fd, payload.data(), payload.size(), 0, reinterpret_cast<sockaddr*>(&bcast),
                       sizeof(bcast));
            } else {
                const std::string ok = gdmPayload(false);
                const std::string hello = gdmPayload(true);
                for (const auto& d : dests)
                    sendFrom(d, d.hello ? hello : ok);
            }
        }
    }
    close(fd);
}

void Companion::httpLoop() {
    int fd = httpListenFd_;
    if (fd < 0) {
        log("HTTP: listen fd missing");
        return;
    }

    // Per-request latency (FPGA Bug 03:24): recv time, path, header and handler ms, WARN at
    // >= 1 s. This thread serves every request in turn, so one slow request delays all later ones.
    CompanionReqLog reqLog;
    const auto steadyMs = []() -> int64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    const auto wallMs = []() -> int64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    };
    struct ReqTimer {  // logs when the loop iteration ends (every continue does that)
        const Companion* self;
        CompanionReqLog& reqLog;
        std::string path;
        int64_t recvWall, acceptMs, hdrMs;
        int64_t (*now)();
        ~ReqTimer() {
            const int64_t end = now();
            if (auto line = reqLog.observe(path, recvWall, hdrMs - acceptMs, end - hdrMs, end))
                self->log(*line);
        }
    };

    // SEC-M2: LAN-only by default (MPX_COMPANION_ALLOW=any|cidr,list|lan) plus a browser Origin allow-list
    // (MPX_COMPANION_ORIGINS=any|host,suffix,list on top of plex.tv/plex.direct/private/localhost/.local).
    const char* allowEnv = getenv("MPX_COMPANION_ALLOW");
    const guard::PeerPolicy peerPolicy = guard::parsePeerPolicy(allowEnv ? allowEnv : "");
    const char* originsEnv = getenv("MPX_COMPANION_ORIGINS");
    const std::string extraOrigins = originsEnv ? originsEnv : "";
    const char* hostsEnv = getenv("MPX_COMPANION_HOSTS");
    const std::string extraHosts = hostsEnv ? hostsEnv : "";
    const char* noOriginEnv = getenv("MPX_COMPANION_NO_ORIGIN");
    const std::string noOriginMode = noOriginEnv ? noOriginEnv : "";
    int64_t lastDenyLogMs = -100000;
    auto logDenied = [&](const char* what, const std::string& who, const std::string& detail) {
        const int64_t now = steadyMs();
        if (now - lastDenyLogMs < 30000)
            return;  // rate limit: a scanner must not flood the log
        lastDenyLogMs = now;
        log(std::string("HTTP: refused ") + what + " from " + who + (detail.empty() ? "" : " " + detail.substr(0, 90)));
    };

    while (running_.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{0, 200000};
        if (select(fd + 1, &rfds, nullptr, nullptr, &tv) <= 0)
            continue;
        sockaddr_in peer{};
        socklen_t peerLen = sizeof(peer);
        int c = accept(fd, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (c < 0)
            continue;
#ifndef MUT_COMPANION_NO_PEER_GUARD
        if (!guard::peerAllowed(ntohl(peer.sin_addr.s_addr), peerPolicy)) {
            char deniedBuf[INET_ADDRSTRLEN]{};
            const char* deniedText = inet_ntop(AF_INET, &peer.sin_addr, deniedBuf, sizeof(deniedBuf));
            logDenied("non-LAN peer", deniedText ? deniedText : "?", "");
            close(c);
            continue;
        }
#endif
        const int64_t acceptWall = wallMs();
        const int64_t acceptMs = steadyMs();
        char peerAddressBuf[INET_ADDRSTRLEN]{};
        const char* peerAddressText =
            inet_ntop(AF_INET, &peer.sin_addr, peerAddressBuf, sizeof(peerAddressBuf));
        const std::string peerAddress = peerAddressText ? peerAddressText : "";
        std::string req;
        const HttpHeaderRead headerRead = recvHttpHeaders(c, req);
        const int64_t hdrDoneMs = steadyMs();
        if (headerRead != HttpHeaderRead::Ok) {
            const char* what = headerRead == HttpHeaderRead::Timeout    ? "(header timeout)"
                               : headerRead == HttpHeaderRead::TooLarge ? "(header too large)"
                               : headerRead == HttpHeaderRead::Closed   ? "(closed)"
                                                                         : "(header error)";
            ReqTimer timer{this, reqLog, what, acceptWall, acceptMs, hdrDoneMs, +steadyMs};
            if (headerRead == HttpHeaderRead::Timeout) {
                log("HTTP: header read timeout");
                sendHttp(c, machineId_, 408, "text/plain", "header timeout\n");
            } else if (headerRead == HttpHeaderRead::TooLarge) {
                log("HTTP: header block exceeds cap");
                sendHttp(c, machineId_, 413, "text/plain", "headers too large\n");
            } else if (headerRead == HttpHeaderRead::Error) {
                log("HTTP: header read error");
            }
            close(c);
            continue;
        }
        ReqTimer timer{this, reqLog, companionRequestPath(req), acceptWall, acceptMs, hdrDoneMs,
                       +steadyMs};

        // SEC-M2: a browser page on an arbitrary site must not drive the player (drive-by cast/stop). Native
        // Plex clients and the phone send no Origin; Plex Web (plex.tv / plex.direct / LAN PMS) is allowed.
        // SEC-M5: Host must be the daemon itself (DNS rebinding), Origin "null" is refused, and cross-site
        // fetches without an Origin are refused; MPX_COMPANION_NO_ORIGIN=reads confines no-Origin clients to reads.
        std::string corsOrigin;
        std::string connLocalIp;
        g_corsOrigin.clear();
        {
            const std::string origin = headerValue(req, "Origin");
            auto deny403 = [&](const char* what, const std::string& detail, const char* body) {
                logDenied(what, peerAddress, detail);
                const std::string text = std::string(body) + "\n";
                (void)sendAll(c, "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\nContent-Length: " +
                                     std::to_string(text.size()) + "\r\nConnection: close\r\n\r\n" + text);
                close(c);
            };
#ifndef MUT_COMPANION_NO_HOST_GUARD
            char ownBuf[INET_ADDRSTRLEN]{};
            sockaddr_in own{};
            socklen_t ownLen = sizeof(own);
            std::string ownIp;
            if (getsockname(c, reinterpret_cast<sockaddr*>(&own), &ownLen) == 0 &&
                inet_ntop(AF_INET, &own.sin_addr, ownBuf, sizeof(ownBuf)))
                ownIp = ownBuf;
            connLocalIp = ownIp;
            const std::string hostHdr = headerValue(req, "Host");
            if (!guard::hostHeaderAllowed(hostHdr, ownIp, extraHosts,
                                            guard::isLanIpv4(ntohl(peer.sin_addr.s_addr)))) {
                deny403("Host header", "host=" + hostHdr, "host not allowed");
                continue;
            }
#endif
#ifndef MUT_COMPANION_NO_FETCH_GUARD
            if (!guard::fetchSiteAllowed(origin, headerValue(req, "Sec-Fetch-Site"))) {
                deny403("cross-site fetch", "", "cross-site not allowed");
                continue;
            }
#endif
#ifndef MUT_COMPANION_NO_ORIGIN_POLICY
            const bool clientHdr =
#ifdef MUT_COMPANION_NO_CLIENT_HDR
                true;
#else
                guard::clientHeaderOk(headerValue(req, "X-Plex-Client-Identifier")) ||
                guard::clientHeaderOk(headerValue(req, "X-Plex-Target-Client-Identifier")) ||
                guard::clientHeaderOk(headerValue(req, "X-Plex-Token")) ||
                guard::clientHeaderOk(headerValue(req, "X-MPX-Client"));
#endif
            // GET /misterplex/status is already read-only. POST /misterplex/update is the
            // one-tap self-update (issue #59): the per-boot nonce is the CSRF check, and the
            // router refuses a browser Origin that is not http://<Host>. A LAN client with no
            // Origin still reaches it. Host, fetch-site, and Origin checks above and below
            // still apply. No starter registered: the route answers "not available" and does
            // not call updateStart_.
            const bool phoneStub = phone_status::parseTarget(req).ours;
            if (!phoneStub && origin.empty() &&
                !guard::noOriginAllowed(req.substr(0, req.find("\r\n")), noOriginMode, clientHdr)) {
                deny403("request without Origin", "no client header on a state-changing path",
                        "origin or client header required");
                continue;
            }
#endif
            corsOrigin = guard::corsEchoOrigin(origin);
            g_corsOrigin = corsOrigin;
#ifndef MUT_COMPANION_NO_ORIGIN_GUARD
            if (!guard::originAllowed(origin, extraOrigins)) {
                logDenied("browser origin", peerAddress, "origin=" + origin);
                static const char kDeny[] =
                    "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\nContent-Length: 19\r\n"
                    "Connection: close\r\n\r\norigin not allowed\n";
                (void)sendAll(c, std::string(kDeny));
                close(c);
                continue;
            }
#endif
        }

        // LAN_AUTH (default off): /misterplex/* control routes need X-MPX-Auth from a
        // non-loopback peer. Plex companion routes (Plex app / Plex Web) are exempt.
        // The secret is never logged.
        if (misterplex::lan_auth::enabled()) {
            const std::string line0 = req.substr(0, req.find("\r\n"));
            const auto sp1 = line0.find(' ');
            const auto sp2 = sp1 == std::string::npos ? std::string::npos : line0.find(' ', sp1 + 1);
            const std::string method = sp1 == std::string::npos ? std::string() : line0.substr(0, sp1);
            std::string target = sp2 == std::string::npos ? std::string() : line0.substr(sp1 + 1, sp2 - sp1 - 1);
            const auto qpos = target.find('?');
            if (qpos != std::string::npos)
                target.erase(qpos);
            const bool peerLoop = (ntohl(peer.sin_addr.s_addr) >> 24) == 127;
            const int verdict = misterplex::lan_auth::companionCheck(
                true, misterplex::lan_auth::secret(), peerLoop, method, target,
                headerValue(req, misterplex::lan_auth::kHeader));
            if (verdict != 0) {
                logDenied("LAN_AUTH", peerAddress, target + " (auth REDACTED)");
                const std::string text = verdict == 401 ? "auth required\n" : "auth refused\n";
                (void)sendAll(c, std::string(verdict == 401 ? "HTTP/1.1 401 Unauthorized" : "HTTP/1.1 403 Forbidden") +
                                     "\r\nContent-Type: text/plain\r\nContent-Length: " +
                                     std::to_string(text.size()) + "\r\nConnection: close\r\n\r\n" + text);
                close(c);
                continue;
            }
        }

        // GET /misterplex/mast (MAST R5b follow-up, FPGA Bug 01:28/03:52): read-only status from
        // the tmpfs reload marker, answered before any other handling so it changes no state.
        // A 200 can carry marker=invalid (N1): read marker= first (mast_status_http.hpp).
        if (const auto mastStatus = mast::statusRequest(req)) {
            sendHttp(c, machineId_, mastStatus->code, "text/plain", mastStatus->body);
            close(c);
            continue;
        }
        if (req.find("GET /misterplex/caption") != std::string::npos) {
            sendHttp(c, machineId_, 200, "text/plain", captionDumpStore() + "\n");
            close(c);
            continue;
        }

        // POST /misterplex/queue/add (frontier item 2): reached only after the SEC-M2/M5 checks above. The PMS owns the
        // queue; this only forwards one add for the bound play queue and answers a fixed text.
        if (const auto qa = qadd::parseQueueAddRequest(req); qa.matched) {
            qadd::Result res;
            if (qa.code != 0) {
                res.code = qa.code;
                res.text = qa.code == 405 ? "POST only" : "Bad request";
            } else {
                qadd::Context ctx;
                qadd::Transport transport;
                QueueNoticeFn notice;
                {
                    std::lock_guard<std::mutex> lock(mu_);
#ifdef MUT_QADD_HTTP_PLAYING_IGNORED
                    ctx.playing = true;
#else
                    ctx.playing = state_ == "playing" || state_ == "paused" || state_ == "buffering";
#endif
                    ctx.playQueueId = pendingPlayQueueId_;
                    ctx.machineId = serverMachineId_;
                    transport = queueAddTransport_;
                    notice = queueAddNotice_;
                }
                ctx.ratingKey = qa.ratingKey;
                res = qadd::runQueueAdd(ctx, qa.mode, transport);
                log("queue add " + std::string(qa.mode == qadd::Mode::PlayNext ? "next" : "append") + " -> " +
                    std::to_string(res.code) + " " + res.text);
#ifndef MUT_QADDN_NO_NOTICE
                if (notice) {   // a finished request tells the TV (Playing next / Queue refused / ...)
                    const std::string t = qadd::noticeText(res);
                    if (!t.empty())
                        notice(t);
                }
#endif
            }
            const char* reason = res.code == 200 ? "OK" : (res.code == 405 ? "Method Not Allowed" : "Error");
            const std::string text = res.text + "\n";
            (void)sendAll(c, "HTTP/1.1 " + std::to_string(res.code) + " " + reason +
                                 "\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(text.size()) +
                                 "\r\nConnection: close\r\n\r\n" + text);
            close(c);
            continue;
        }

        // GET /misterplex/status (issue #57): one HTML page, answered after the companion guard and
        // before cast handling so it changes no playback state. POST /misterplex/update (issue #59)
        // calls updateStart_ only when the router sets startUpdate. The refresh loop never does.
        if (phone_status::parseTarget(req).ours) {
            const phone_status::BodyNeed need = phone_status::updateBodyNeed(req);
            if (need.read && !need.tooLarge && need.more > 0) {
                std::string extra;
                if (readExact(c, extra, need.more, 1000))
                    req += extra;
            }
            phone_status::Facts facts;
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (phoneNonce_.empty())
                    phoneNonce_ = phone_status::bootNonce();
                facts.version = phoneVersion_;
                facts.commit = phoneCommit_;
                facts.md5 = phoneMd5_;
                facts.core = phoneCore_;
                facts.pairing = phonePairing_;
                facts.title = phoneTitle_;
                facts.position = phone_status::formatPosition(timeMs_, durationMs_);
                facts.mode = state_.empty() ? "stopped" : state_;
                facts.helper = phoneHelper_;
                facts.newer = phoneNewer_;
                facts.playing =
                    state_ == "playing" || state_ == "paused" || state_ == "buffering";
                facts.canUpdate = static_cast<bool>(updateStart_);
                facts.nonce = phoneNonce_;
                facts.reportCode = misterplex::bootReportCode();
            }
            if (const auto phone = phone_status::route(req, facts)) {
                if (phone->startUpdate) {
                    UpdateStartFn start;
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        start = updateStart_;
                    }
                    bool started = false;
                    if (start) {
                        try {
                            started = start();
                        } catch (...) {
                            log("phone update start exception");
                        }
                    }
                    if (started) {
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            phoneNonce_.clear();
                        }
                        log("phone update started");
                        sendHttp(c, machineId_, 202, "text/plain", "update started\n");
                    } else if (start) {
                        sendHttp(c, machineId_, 409, "text/plain", "update already running\n");
                    } else {
                        sendHttp(c, machineId_, 503, "text/plain", "update: not available\n");
                    }
                    close(c);
                    continue;
                }
                sendHttp(c, machineId_, phone->code, phone->contentType, phone->body);
                if (phone->code == 200 && phone_status::parseTarget(req).status)
                    log(phone_status::statusLogLine(facts));
                close(c);
                continue;
            }
        }

        const std::string requestControllerId = controllerId(req);
        const std::string requestCommandId =
            timelineCommandFor(requestControllerId, peerAddress,
                               queryParam(req, "commandID"));
        if (pollDiag_ && req.find("/player/") != std::string::npos) {
            log("companion: diag " + companionRequestPath(req) + " peer=" + peerAddress +
                " client=" + (requestControllerId.empty() ? "-" : requestControllerId) +
                " target=" + headerValue(req, "X-Plex-Target-Client-Identifier") +
                " cmd_in=" + queryParam(req, "commandID") + " cmd_out=" + requestCommandId +
                " wait=" + queryParam(req, "wait"));
        }

        // Refresh PMS timeline auth only from cast *control* requests that carry the
        // playing server's token (playMedia/seek/playback/*). Do NOT take tokens from
        // timeline poll/subscribe: Plex Web long-polls with the *controller* PMS token
        // (often the controller's own private LAN) while playMedia used a remote plex.direct transient
        // token — overwriting it yields /:/timeline HTTP 401 and Web scrubber stuck
        // at 0:00 (L41 follow-up; user rk 154219/154269 on machine 1cdd…).
        if (onTokenUpdate_ && req.find("/player/") != std::string::npos) {
            const std::string tok = castControlRefreshToken(req);
            {
                if (!tok.empty()) {
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        pendingToken_ = tok;
                    }
                    try {
                        if (tokenRefreshesActiveSession(req))
                            onTokenUpdate_(tok);
                        else if (onPlayMediaToken_)
                            onPlayMediaToken_(tok);
                    } catch (...) {
                        log("token update handler exception");
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            if (req.find("/player/") != std::string::npos || req.find("/resources") != std::string::npos)
                castBound_ = true;
        }

        if (req.find("OPTIONS") == 0) {
            sendHttp(c, machineId_, 200, "text/plain", "",
                     headerValue(req, "Access-Control-Request-Headers"));
            close(c);
            continue;
        }

        if (req.find("GET /resources") != std::string::npos ||
            req.find("GET /identity") != std::string::npos) {
            sendHttp(c, machineId_, 200, "application/xml", resourcesXml(connLocalIp));
            close(c);
            continue;
        }

        if (plexUiNavEnabled_ && req.find("/player/navigation/") != std::string::npos) {
            std::string op;
            if (req.find("moveUp") != std::string::npos)
                op = "up";
            else if (req.find("moveDown") != std::string::npos)
                op = "down";
            else if (req.find("moveLeft") != std::string::npos)
                op = "left";
            else if (req.find("moveRight") != std::string::npos)
                op = "right";
            else if (req.find("/select") != std::string::npos)
                op = "select";
            else if (req.find("/back") != std::string::npos)
                op = "back";
            else if (req.find("/home") != std::string::npos)
                op = "home";
            else if (req.find("uiSize") != std::string::npos || req.find("ui-size") != std::string::npos)
                op = "uisize";
            if (!op.empty() && onPlexUiNav_)
                onPlexUiNav_(op);
            sendHttp(c, machineId_, 200, "application/xml", timelineXml(requestCommandId, requestControllerId, peerAddress));
            close(c);
            continue;
        }

        if (req.find("/player/timeline/subscribe") != std::string::npos) {
            std::string protocol = queryParam(req, "protocol");
            if (protocol.empty())
                protocol = "http";
            std::transform(protocol.begin(), protocol.end(), protocol.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            const uint16_t subscriberPort = callbackPort(req);
            std::string callbackHost = peerAddress;
            const std::string addrParam = pctDecode(queryParam(req, "address"));
            // Prefer a non-loopback address= when present (PMS relay). Loopback is the
            // Web/PMS host's view of itself: POSTing 127.0.0.1 from the MiSTer never reaches it.
            if (!addrParam.empty() && !subscriberHostIsLoopback(addrParam))
                callbackHost = addrParam;
            if (protocol == "http" && subscriberPort != 0 && !callbackHost.empty() &&
                !subscriberHostIsLoopback(callbackHost)) {
#ifdef MUT_TIMELINE_SUBSCRIBE_PEER_IS_CALLBACK
                subscribeTimeline(requestControllerId, callbackHost, protocol, subscriberPort,
                                  requestCommandId, callbackHost);
#else
                subscribeTimeline(requestControllerId, callbackHost, protocol, subscriberPort,
                                  requestCommandId, peerAddress);
#endif
            } else {
                log("timeline: rejected invalid subscriber endpoint host=" + callbackHost +
                    " port=" + std::to_string(subscriberPort));
            }
            sendHttp(c, machineId_, 200, "application/xml",
                     timelineXml(requestCommandId, requestControllerId, peerAddress));
            close(c);
            continue;
        }

        // Unsubscribe: drop cast-bound hold so idle polls can go pure stopped.
        if (req.find("/player/timeline/unsubscribe") != std::string::npos) {
            const bool noSubscribers =
                unsubscribeTimeline(requestControllerId, peerAddress);
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (noSubscribers)
                    castBound_ = false;
                if (noSubscribers && !wantPlay_) {
                    prePlayHold_ = false;
                    disarmHoldLocked();
                }
            }
            sendHttp(c, machineId_, 200, "application/xml",
                     timelineXml(requestCommandId, requestControllerId, peerAddress));
            close(c);
            continue;
        }

        // Timeline poll / proxy alias — never auto-start media from poll.
        if (req.find("/player/timeline/poll") != std::string::npos ||
            req.find("/player/proxy/timeline") != std::string::npos ||
            (req.find("/timeline") != std::string::npos && req.find("playMedia") == std::string::npos &&
             req.find("mirror") == std::string::npos && req.find("unsubscribe") == std::string::npos)) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                castBound_ = true;
                // Live poller ⇒ Web still has us as cast target. prePlayHold_ here only
                // guards late teardown progress; a poll never extends the wire hold.
                if (!wantPlay_ && !prePlayHold_)
                    prePlayHold_ = true;
            }
            {
                // Long-hold (no-proxy commit b): hold wait=1 polls for a new timeline, never past
                // MPX_COMPANION_POLL_HOLD_MS (default 1000), and yield the moment another connection
                // is waiting (this accept loop is single-threaded).
                const int holdMs =
                    pollHoldMs(queryParam(req, "wait"), std::getenv("MPX_COMPANION_POLL_HOLD_MS"));
                const uint64_t genAtPoll = timelineGen_.load();
                holdPoll(
                    holdMs,
                    [&] {
                        if (timelineGen_.load() != genAtPoll || !running_.load())
                            return true;
                        fd_set pr;
                        FD_ZERO(&pr);
                        FD_SET(fd, &pr);
                        timeval z{0, 0};
                        return select(fd + 1, &pr, nullptr, nullptr, &z) > 0;
                    },
                    steadyMs,
                    [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); });
            }
            const std::string pollXml = timelineXml(requestCommandId, requestControllerId, peerAddress);
            if (pollDiag_)
                log("companion: diag poll reply " + timelineDiagSummary(pollXml));
            sendHttp(c, machineId_, 200, "application/xml", pollXml);
            close(c);
            continue;
        }

        // Mirror: stage identity + prePlayHold (no media start). Do not demote live cast.
        if (req.find("mirror") != std::string::npos && req.find("playMedia") == std::string::npos) {
            PlayRequest pr = parsePlayRequest(req);
            pr.peerAddress = peerAddress; // source IP of this request: an IPv4 PMS-address fallback
            bool mirrorStaged = false;
            {
                std::lock_guard<std::mutex> lock(mu_);
                const bool keepActive =
                    wantPlay_ && (state_ == "playing" || state_ == "buffering" || state_ == "paused");
                // BUG2-RECAST: once a finished queue has decayed to idle, a new mirror is a new cast
                // (Web needs the pre-play buffering hold or it closes its fresh session before playMedia).
                if (!keepActive && (!terminalStop_ || endedIdleLocked())) {
                    // Remember key for following playMedia; wire omits media bind
                    // while wantPlay_ is false (buffering@navigation hold).
                    if (!pr.key.empty()) {
                        pendingKey_ = pr.key;
                        idleKey_.clear();   // a new bind replaces the stopped item identity
                    }
                    mirrorStaged = true;
                    if (!pr.ratingKey.empty())
                        pendingRatingKey_ = pr.ratingKey;
                    if (!pr.address.empty()) {
                        bindTimelinePmsLocked(pr.address, pr.port, pr.protocol);
                    } else {
                        if (!pr.port.empty())
                            serverPort_ = pr.port;
                        if (!pr.protocol.empty())
                            serverProto_ = pr.protocol;
                    }
                    if (!pr.serverMachineId.empty())
                        serverMachineId_ = pr.serverMachineId;
                    if (!pr.peerAddress.empty())
                        serverPeer_ = pr.peerAddress;
                    // Drop stale queue so hold never looks like a live session if
                    // wantPlay latches incorrectly; restage only valid play-queue.
                    pendingPlayQueueId_.clear();
                    pendingPlayQueueItemId_.clear();
                    pendingContainerKey_.clear();
                    if (!pr.playQueueId.empty())
                        pendingPlayQueueId_ = pr.playQueueId;
                    if (!pr.playQueueItemId.empty())
                        pendingPlayQueueItemId_ = pr.playQueueItemId;
                    if (!pr.containerKey.empty() &&
                        pr.containerKey.find("/playQueues/") != std::string::npos)
                        pendingContainerKey_ = pr.containerKey;
                    durationMs_ = 0;
                    pendingGeneration_ = 0;
                    prePlayHold_ = true;
                    idleKey_.clear();   // a mirror hold is about the NEW item, not the stopped one
                    // Web mirrors every detail page; each mirror refreshes a bounded hold.
                    armHoldLocked(mirrorHoldMs_);
                    wantPlay_ = false;
                    state_ = "stopped"; // wire shows buffering until holdUntil_
                    terminalStop_ = false;
                    castBound_ = true;
                }
                // else: leave live timeline alone (Web mirror after playMedia must not idle)
            }
            // The mirror client is the owner of whatever token this stage left.
            // The previous owner is not sticky: the next poll reads this owner.
            if (mirrorStaged) {
#ifdef MUT_MIRROR_SETCASTOWNER
                setCastOwner(requestControllerId, peerAddress);
#else
                noteCastOwner(requestControllerId, peerAddress, pr.token);
#endif
            }
            requestTimelinePush(true);
            sendHttp(c, machineId_, 200, "application/xml",
                     timelineXml(requestCommandId, requestControllerId, peerAddress));
            log("mirror staged key=" + pr.key);
            close(c);
            continue;
        }

        // ZAPAROO default off. A card text that names /library/metadata/<ratingKey>
        // is rewritten into playMedia so onPlay_ resolves it and playOwenbRoute
        // starts it. Off, or a card that is not a library item: answer and return.
        // The card text is not logged.
        {
            const misterplex::zap::Decision zap = misterplex::zap::decide(req);
            if (zap.matched && !zap.startPlay) {
                log(zap.log);
                sendHttp(c, machineId_, zap.code, "text/plain", zap.body);
                close(c);
                continue;
            }
            if (zap.matched && zap.startPlay) {
                log(zap.log);
                const auto nl = req.find("\r\n");
                const std::string rest = nl == std::string::npos ? std::string("\r\n") : req.substr(nl);
                req = misterplex::zap::playMediaRequestLine(zap) + rest;
            }
        }

        if (req.find("playMedia") != std::string::npos ||
            req.find("/player/playback/") != std::string::npos) {
            const bool isPlayMedia = req.find("playMedia") != std::string::npos;
            const bool isPause = req.find("/pause") != std::string::npos ||
                                 req.find("playback/pause") != std::string::npos;
            const bool isStop = req.find("/stop") != std::string::npos ||
                                req.find("playback/stop") != std::string::npos;
            const bool isSeek = req.find("seekTo") != std::string::npos ||
                                (req.find("/seek") != std::string::npos &&
                                 req.find("seekTo") == std::string::npos &&
                                 req.find("step") == std::string::npos);
            // Relative scrubber steps (Web remote / keyboard)
            const bool isStepForward = req.find("stepForward") != std::string::npos;
            const bool isStepBack = req.find("stepBack") != std::string::npos;
            const bool isSkipNext = req.find("skipNext") != std::string::npos;
            const bool isSkipPrevious = req.find("skipPrevious") != std::string::npos;
            const bool isResumePlay =
                !isPlayMedia && !isPause && !isStop && !isSeek && !isStepForward &&
                !isStepBack && !isSkipNext && !isSkipPrevious &&
                (req.find("/player/playback/play") != std::string::npos ||
                 req.find("playback/play?") != std::string::npos ||
                 req.find("playback/play ") != std::string::npos);

            if (isPlayMedia) {
                PlayRequest pr = parsePlayRequest(req);
                if (pr.targetRejected)
                    log("playMedia: target address/protocol/port refused (not plain http(s) host:port or link-local); using configured server");
                pr.peerAddress = peerAddress; // source IP of this request: an IPv4 PMS-address fallback
                if (pr.key.empty())
                    pr.key = "(no-key)";
                bool inPlaceStreams = false;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    inPlaceStreams =
                        playMediaIsInPlaceStreamChange(wantPlay_, state_, pendingKey_, timeMs_, pr);
                }
                if (inPlaceStreams) {
                    SetStreamsRequest asked;
                    asked.hasAudio = pr.hasAudioStreamId;
                    asked.audioStreamId = pr.audioStreamId;
                    asked.hasSubtitle = pr.hasSubtitleStreamId;
                    asked.subtitleStreamId = pr.subtitleStreamId;
                    applyAndEchoStreams(asked);
                    requestTimelinePush(true);
                    sendHttp(c, machineId_, 200, "application/xml", timelineXml(requestCommandId, requestControllerId, peerAddress));
                    close(c);
                    log("companion: playMedia in-place setStreams key=" + pr.key +
                        " audio=" + (pr.hasAudioStreamId ? std::to_string(pr.audioStreamId) : std::string("-")) +
                        " subtitle=" +
                        (pr.hasSubtitleStreamId ? std::to_string(pr.subtitleStreamId) : std::string("-")));
                    continue;
                }
#ifdef MUT_PLAYMEDIA_SETCASTOWNER
                setCastOwner(requestControllerId, peerAddress);
#else
                noteCastOwner(requestControllerId, peerAddress, pr.token);
#endif
                // Serialize generation assignment before publishing this request
                // as pending. The callback shares the player's handoff mutex, so
                // an older handler either finishes first or sees this generation;
                // it can never publish stale DAR after the new request is staged.
                if (onPlayQueued_) {
                    try {
                        pr.dispatchGeneration = onPlayQueued_();
                    } catch (...) {
                        log("playQueued handler exception");
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    wantPlay_ = true;
                    stopDuringResolve_ = false;
                    userStop_ = false;
                    prePlayHold_ = false;
                    // A new item: the previous episode's stream ids must not stick on this poll.
                    streamsKnown_ = false;
                    audioStreamId_ = 0;
                    subtitleStreamId_ = 0;
                    disarmHoldLocked();
                    castBound_ = true;
                    state_ = "buffering";
                    terminalStop_ = false;
                    // Never plant negative scrubber time (Web/browse edge).
                    int64_t off = pr.offsetMs < 0 ? 0 : pr.offsetMs;
                    if (resumeAfterRespawn_ > 0) {
                        off = resumeAfterRespawn_;
                        resumeAfterRespawn_ = 0;
                    }
                    if (!pr.offsetPresent && lastGoodPositionMs_ > 0 &&
                        !lastGoodKey_.empty() && (pr.key.empty() || pr.key == lastGoodKey_))
                        off = lastGoodPositionMs_;
                    pr.offsetMs = off;
                    if (off > 0)
                        pr.offsetPresent = true;
                    // Drop prior title duration on every fresh cast. A shorter leftover
                    // duration (e.g. a short clip) must not clamp a legitimate continue-
                    // watching offset on a longer next title. bindMedia re-supplies
                    // duration after resolve and clamps timeMs_ then.
                    durationMs_ = 0;
                    timeMs_ = off;
                    scrubTargetMs_ = off; // hold until bind/demux
                    lastGoodPositionMs_ = 0;
                    lastGoodKey_.clear();
                    pendingKey_ = pr.key;
                    idleKey_.clear();   // a new bind replaces the stopped item identity
                    // Token is installed by noteCastOwner: a new owner with no token=
                    // must not inherit the previous owner's pendingToken_.
                    pendingGeneration_ = pr.dispatchGeneration;
                    pendingContainerKey_ = pr.containerKey;
                    const std::string prevQueueId = pendingPlayQueueId_;
                    const std::string prevQueueVersion = pendingPlayQueueVersion_;
                    pendingPlayQueueId_ = pr.playQueueId;
                    pendingPlayQueueItemId_ = pr.playQueueItemId;
                    pr.playQueueVersion = playQueueVersionAfterReplace(
                        prevQueueId, prevQueueVersion, pr.playQueueId, pr.playQueueVersion);
                    pendingPlayQueueVersion_ = pr.playQueueVersion;
                    // Synthetic containerKey when only playQueueID was supplied so
                    // auto-next / skipNext lastPlay.containerKey paths stay queue-shaped.
                    if (pendingContainerKey_.empty() && !pendingPlayQueueId_.empty())
                        pendingContainerKey_ = "/playQueues/" + pendingPlayQueueId_ + "?own=1";
                    // Mirror onto PlayRequest so async onPlay_/lastPlay see the queue bind
                    // (pending* alone is display-only until bindMedia).
                    if (pr.containerKey.empty() && !pr.playQueueId.empty())
                        pr.containerKey = "/playQueues/" + pr.playQueueId + "?own=1";
                    if (!pr.ratingKey.empty())
                        pendingRatingKey_ = pr.ratingKey;
                    if (!pr.address.empty()) {
                        bindTimelinePmsLocked(pr.address, pr.port, pr.protocol);
                    } else {
                        if (!pr.port.empty())
                            serverPort_ = pr.port;
                        if (!pr.protocol.empty())
                            serverProto_ = pr.protocol;
                    }
                    if (!pr.serverMachineId.empty())
                        serverMachineId_ = pr.serverMachineId;
                    if (!pr.peerAddress.empty())
                        serverPeer_ = pr.peerAddress;
                }
                requestTimelinePush(true);
                int64_t ackOff = 0;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    ackOff = timeMs_;
                }
                const std::string ackXml = timelineXml(requestCommandId, requestControllerId, peerAddress);
                if (pollDiag_)
                    log("companion: diag playMedia reply " + timelineDiagSummary(ackXml));
                sendHttp(c, machineId_, 200, "application/xml", ackXml);
                close(c);
                log("playMedia ACK key=" + pr.key + " offMs=" + std::to_string(ackOff));
                log("companion: stream requested key=" + pr.key + " no client profile");
                if (onPlay_) {
                    std::thread([this, pr]() {
                        try {
                            onPlay_(pr);
                        } catch (...) {
                            log("playMedia handler exception");
                        }
                    }).detach();
                }
                const std::string policy = titlePlayPolicy(pr);
                log("playMedia title=" + pr.key + " ratingKey=" + pr.ratingKey +
                    " policy=" + policy);
                continue;
            }

            // Plex Web / app: /player/playback/setStreams?audioStreamID=&subtitleStreamID=
            // subtitleStreamID=0 is off. The ACK timeline echoes the ids the player kept.
            if (req.find("playback/setStreams") != std::string::npos) {
                applyAndEchoStreams(parseSetStreams(req));
                requestTimelinePush(true);
                sendHttp(c, machineId_, 200, "application/xml", timelineXml(requestCommandId, requestControllerId, peerAddress));
                close(c);
                continue;
            }
            // OWENB-P1B T5: setParameters?volume=N: the player's volume (OB: the core's attenuation).
            if (req.find("playback/setParameters") != std::string::npos) {
                const std::string v = queryParam(req, "volume");
                if (!v.empty() && std::isdigit(static_cast<unsigned char>(v[0]))) {
                    const int vol = std::clamp(std::atoi(v.c_str()), 0, 100);
                    volume_.store(vol);
                    if (log_)
                        log_("companion: setParameters volume=" + std::to_string(vol));
                    if (onVolume_)
                        onVolume_(vol);
                    requestTimelinePush(true);
                }
                sendHttp(c, machineId_, 200, "application/xml", timelineXml(requestCommandId, requestControllerId, peerAddress));
                close(c);
                continue;
            }
            if (isPause || isResumePlay) {
                TransportRequest request;
                bool accepted = false;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    accepted = acceptPauseResumeLocked(isPause, request);
                }
                if (accepted)
                    requestTimelinePush(true);
                sendHttp(c, machineId_, 200, "application/xml",
                         timelineXml(requestCommandId, requestControllerId, peerAddress));
                const auto& handler = isPause ? onPause_ : onResume_;
                if (accepted && handler)
                    handler(request);
                close(c);
                continue;
            }
            if (isStop) {
                // Drop bind first so stop ACK is buffering@navigation without keys
                // (video/stopped+key idles Web and freezes scrubber / Resume dialog).
                // clearMedia before player.stop so late progress cannot re-arm wantPlay
                // (setState ignores progress while prePlayHold && !wantPlay).
                const auto request = clearMedia();
                if (onStop_) {
                    const auto handler = onStop_;
                    // Resolve holds playHandoffMu; do not block the HTTP thread on stop().
                    if (deferStop_ && deferStop_()) {
                        std::thread([handler, request]() { handler(request); }).detach();
                    } else {
                        handler(request);
                    }
                }
                sendHttp(c, machineId_, 200, "application/xml",
                         timelineXml(requestCommandId, requestControllerId, peerAddress));
                close(c);
                continue;
            }
            if (isSeek) {
                bool present = false;
                int64_t ms = parseOffsetMs(req, &present);
                TransportRequest request;
                bool accepted = false;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    if (present)
                        accepted = acceptSeekLocked(ms, request);
                }
                if (accepted)
                    requestTimelinePush(true);
                sendHttp(c, machineId_, 200, "application/xml",
                         timelineXml(requestCommandId, requestControllerId, peerAddress));
                if (accepted && onSeek_)
                    onSeek_(request);
                close(c);
                continue;
            }
            if (isStepForward || isStepBack) {
                // Default ±10s; optional offset= is relative step size in ms (cap 120s).
                // offset=0 → keep default (not a zero-step no-op). Negative sizes use abs.
                int64_t step = 10000;
                auto off = queryParam(req, "offset");
                if (!off.empty()) {
                    int64_t v = std::atoll(off.c_str());
                    if (v < 0)
                        v = -v;
                    // Non-zero only; clamp huge values (Web may send large step sizes).
                    if (v > 0)
                        step = (v > 120000) ? 120000 : v;
                }
                if (isStepBack)
                    step = -step;
                int64_t t = 0, d = 0;
                TransportRequest request;
                bool accepted = false;
                int64_t target = 0;
                int64_t applied = 0;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    t = timeMs_;
                    d = durationMs_;
                    target = t + step;
                    if (target < 0)
                        target = 0;
                    if (d > 0 && target > d)
                        target = d;
                    applied = target - t;
                    accepted = acceptSeekLocked(target, request);
                }
                if (accepted)
                    requestTimelinePush(true);
                sendHttp(c, machineId_, 200, "application/xml",
                         timelineXml(requestCommandId, requestControllerId, peerAddress));
                // Prefer absolute seek when available so player lands on clamped target
                // even if positionMs lags companion timeMs_ (progress race).
                if (accepted) {
                    if (onSeek_)
                        onSeek_(request);
                    else if (onStep_)
                        onStep_(applied, request);
                }
                close(c);
                continue;
            }
            if (isSkipNext) {
                bool active = false;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    active = wantPlay_;
                }
                sendHttp(c, machineId_, 200, "application/xml",
                         timelineXml(requestCommandId, requestControllerId, peerAddress));
                // Empty Plex session: ACK only. SPELUNKR sets skipWhenStopped_ so
                // ChannelUp still runs without companion wantPlay_.
                if ((active || skipWhenStopped_) && onSkipNext_)
                    onSkipNext_();
                close(c);
                continue;
            }
            if (isSkipPrevious) {
                int64_t t = 0;
                bool active = false;
                bool seekAccepted = false;
                TransportRequest request;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    t = timeMs_;
                    active = wantPlay_;
                    if (active && onSkipPrevious_)
                        request = transportRequestLocked(TransportCommand::Previous);
                    else if (active && onSeek_ && t != 0)
                        seekAccepted = acceptSeekLocked(0, request);
                }
                // Preserve the accepted position/identity for the restart-or-queue
                // decision; never apply an old optimistic plant after its callback.
                // SPELUNKR skipWhenStopped_: ChannelDown without wantPlay_.
                if ((active || skipWhenStopped_) && onSkipPrevious_)
                    onSkipPrevious_(request);
                else if (active && seekAccepted)
                    onSeek_(request);
                sendHttp(c, machineId_, 200, "application/xml",
                         timelineXml(requestCommandId, requestControllerId, peerAddress));
                close(c);
                continue;
            }

            sendHttp(c, machineId_, 200, "application/xml",
                     timelineXml(requestCommandId, requestControllerId, peerAddress));
            close(c);
            continue;
        }

        sendHttp(c, machineId_, 404, "text/plain", "not found");
        close(c);
    }
    httpReady_.store(false);
    close(fd);
    httpListenFd_ = -1;
}

} // namespace misterplex
