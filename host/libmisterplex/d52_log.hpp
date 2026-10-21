#pragma once
// D52-LOG: a lab-only event log for the D52 raster-shift soak. Requirements: review 17:15 (a)-(d) and 17:22,
// release 17:16-17:24. Basis: the d52 fbpar CRC writers review (1004) (990eb0df) and
// d52-h5b-aspect-tuples-1004.md (26afc216).
//
// It is off unless MPX_D52_LOG=1, and it only logs. It never writes the FPGA, the ring or DDR, and it adds no
// SPI or UIO traffic:
//   - Poller (its own thread; no map or fd of its own: it reads the PLX page through the daemon's one mapper,
//     FpgaSpi::plxPageRead32, under that mapper's lock, review 08:17 (b)): runs only while a ring
//     session is armed (BEGIN acknowledged, until END ok, ring reset or epoch reset). The page is taken at each
//     arm: the daemon's own resolved DDR doorbell page, for a D52-LOG build only (sessionPlxPage: 0x48005D45,
//     0x48005D52 and 0x48009E36, all 0x300FF000). Any other build, or no page: no arm, and one loud line.
//     - PLXJ x/y/token on change, classified against the daemon's own logged PLXA downloads. This is the
//       H1'/H5b discriminator.
//     - PLXF underrun/debug on change (seq is liveness only).
//     - PLXD frames_done gaps and stalls (the DDR proxy for a has_frame gap).
//   - Daemon hooks: every index-4 (PLXA) download and its ACK result; every ring control record that publishes
//     (BEGIN/FLUSH/END/PROBE/DRAIN/PAUSE/RESUME); the ring epoch reset and ring reset; the status[11] flush; and
//     MVPS flag/error_code changes seen by the play loop's existing MVPS read (a committed codec error).
//   - Not readable over DDR, so logged as n/a and never polled: has_frame (SPI telem_flags), FB_PAR/crc (UIO),
//     and visible_w/h and the R2 output (not exported by prod RTL).
// Every line starts "d52 t=<CLOCK_MONOTONIC s> rt=<CLOCK_REALTIME s>", so the soak can align it with the HDMI
// capture. A heartbeat (60 s) carries the counters.

#include "libmisterplex/source_aspect.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MPX_D52_DEVMEM_PATH
#ifdef MPX_DEVMEM_PATH
#define MPX_D52_DEVMEM_PATH MPX_DEVMEM_PATH
#else
#define MPX_D52_DEVMEM_PATH "/dev/mem"
#endif
#endif

namespace misterplex {
namespace d52 {

constexpr const char* kEnv = "MPX_D52_LOG";
constexpr const char* kPollEnv = "MPX_D52_LOG_POLL_MS";
constexpr const char* kGapEnv = "MPX_D52_LOG_GAP_MS";
// Retired (review 00:14 F2): the page is per session (sessionPlxPage). If set, it is not read; the start line
// says so.
constexpr const char* kRetiredPageEnv = "MPX_D52_LOG_PLX_PAGE";
constexpr uint32_t kPlxfOff = 0x118u;          // PLXF frame-store status
constexpr uint32_t kPlxdOff = 0x128u;          // PLXD bank release / frames_done
constexpr uint32_t kPlxjOff = 0x130u;          // PLXJ source-aspect ACK
constexpr uint32_t kPlxfMagic = 0x504C5846u;
constexpr uint32_t kPlxdMagic = 0x504C5844u;
constexpr uint32_t kPlxjMagic = 0x504C584Au;

struct Config {
    bool enabled = false;
    int poll_ms = 5;          // MPX_D52_LOG_POLL_MS, 1..100
    int gap_ms = 60;          // MPX_D52_LOG_GAP_MS, 20..2000: a frames_done gap this long is logged
    int stall_ms = 1000;      // a gap still open this long is also logged once, before it ends
    int absent_ms = 5000;     // all PLX words absent this long: disarm (the Plex core is gone)
    int heartbeat_ms = 60000;
    const char* note = "";    // why an env value was not used
};

// The D52-LOG builds (review 00:52 S1, 00:55; soak-scoped, not the general per-build page table): the three 480p
// soak builds whose PLX page is verified in their own trees. 4c210d99 (Plex_480p.qsf:178, 0x48005D45) and
// 9c86d9b2 (Plex_480p.qsf:184, 0x48005D52) both set PLEX_PRESENT_480P (:162), and so does the
// shipping 9E36 refit (ea4ca1e0 Plex_480p_r1.qsf :199 ID, :162 PLEX_PRESENT_480P; review 14:13 item 6, review 14:13). That present_core branch
// (present_core.sv:905-926 in both trees) publishes PLXF/PLXD/PLXJ at DDR_FRAME_480P_FPGA_* =
// 0x300FF000 + 0x118/0x128/0x130 (ddr_frame_layout_params.svh:30-41). Every other build, in the FPGA build table or
// not, is not armed.
inline bool d52Build(uint32_t build_id) { return build_id == 0x48005D45u || build_id == 0x48005D52u || build_id == 0x48009E36u; }

// The PLX page a session's arm reads (review 00:14 F1, approved 00:20): for a D52-LOG build, the daemon's own
// resolved DDR doorbell page, FpgaSpi's ddrLayout_.doorbell_phys. That is the base of the daemon's own PLXF, PLXD
// and PLXJ reads (+0x118/+0x128/+0x130; readFrameStoreStatus, readSourceAspectAck), and it is 0x300FF000 at the
// 480p DECODE=640x480. 0 = no page (*why says which): no arm.
inline uint32_t sessionPlxPage(uint32_t build_id, uint32_t doorbell_phys, uint32_t phys_base, const char** why) {
    const char* w = nullptr;
    uint32_t page = 0;
    if (!d52Build(build_id))
        w = "not a D52-LOG build";
    else if (doorbell_phys < phys_base || doorbell_phys < 0x30000000u || doorbell_phys > 0x3FFFF000u)
        w = "no resolved DDR doorbell base";
    else if (doorbell_phys & 0xFFFu)
        w = "the DDR doorbell base is not page-aligned";
    else
        page = doorbell_phys;
    if (why)
        *why = w;
    return page;
}

inline Config configFromEnv() {
    Config c;
    const char* e = std::getenv(kEnv);
    c.enabled = e && std::strcmp(e, "1") == 0;
    auto num = [&c](const char* name, int& field, int lo, int hi) {
        const char* v = std::getenv(name);
        if (!v || !*v)
            return;
        char* end = nullptr;
        const long n = std::strtol(v, &end, 10);
        if (end && *end == '\0' && n >= lo && n <= hi)
            field = static_cast<int>(n);
        else
            c.note = " (an MPX_D52_LOG_* value was out of range: default used)";
    };
    num(kPollEnv, c.poll_ms, 1, 100);
    num(kGapEnv, c.gap_ms, 20, 2000);
    if (const char* v = std::getenv(kRetiredPageEnv); v && *v)
        c.note = " (MPX_D52_LOG_PLX_PAGE is not read: the PLX page is the daemon's resolved doorbell page, per arm)";
    return c;
}

struct Stamp {
    int64_t mono_us = 0;
    int64_t real_us = 0;
};

inline Stamp nowStamp() {
    timespec m{}, r{};
    ::clock_gettime(CLOCK_MONOTONIC, &m);
    ::clock_gettime(CLOCK_REALTIME, &r);
    return {static_cast<int64_t>(m.tv_sec) * 1000000 + m.tv_nsec / 1000,
            static_cast<int64_t>(r.tv_sec) * 1000000 + r.tv_nsec / 1000};
}

// One sample of the three PLX words. *_ok: two reads agreed and the magic matched.
struct PlxWords {
    bool j_ok = false, d_ok = false, f_ok = false;
    uint64_t j = 0, d = 0, f = 0;
};

class Logger {
public:
    using Sink = std::function<void(const std::string&)>;
    using Clock = std::function<Stamp()>;

    explicit Logger(const Config& c, Sink sink = nullptr, Clock clock = nullptr)
        : cfg_(c), sink_(sink ? std::move(sink) : Sink([](const std::string& s) {
              std::fprintf(stderr, "%s\n", s.c_str());
          })),
          clock_(clock ? std::move(clock) : Clock([] { return nowStamp(); })) {}

    const Config& config() const { return cfg_; }
    bool armed() const { return armed_.load(); }

    void start(int pid) {
        char b[256];
        std::snprintf(b, sizeof b,
                      "start pid=%d poll_ms=%d gap_ms=%d stall_ms=%d plx_page=at_arm (each arm line names its "
                      "page) (a new daemon process: its first ring CTRL is a generation clear)%s",
                      pid, cfg_.poll_ms, cfg_.gap_ms, cfg_.stall_ms, cfg_.note);
        std::lock_guard<std::mutex> g(mu_);
        emit(b);
    }

    // ---- daemon hooks (any thread) ----
    void ringRecord(const char* event, uint64_t session, uint32_t write_count) {
        char b[160];
        std::snprintf(b, sizeof b, "ring %s published session=0x%llx count=%u", event,
                      static_cast<unsigned long long>(session), write_count);
        std::lock_guard<std::mutex> g(mu_);
        ++ring_events_;
        emit(b);
    }
    void ringFence(const char* event, uint64_t session, bool ok) {
        char b[160];
        std::snprintf(b, sizeof b, "ring %s fence=%s session=0x%llx", event, ok ? "ok" : "failed",
                      static_cast<unsigned long long>(session));
        std::lock_guard<std::mutex> g(mu_);
        emit(b);
    }
    void ringReset(const char* what, int epoch, const char* result) {
        char b[160];
        std::snprintf(b, sizeof b, "ring %s epoch=%d result=%s", what, epoch, result);
        std::lock_guard<std::mutex> g(mu_);
        ++ring_events_;
        emit(b);
    }
    void statusFlush(bool ok) {
        std::lock_guard<std::mutex> g(mu_);
        ++ring_events_;
        emit(std::string("status FLUSH status[11] pulse result=") + (ok ? "ok" : "failed"));
    }
    // Every index-4 download, whatever the caller, logged before the transfer.
    void plxaDownload(const uint8_t* p, size_t len) {
        char b[192];
        std::lock_guard<std::mutex> g(mu_);
        ++sends_;
        if (p && len >= 9) {
            const uint32_t magic = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
            send_x_ = static_cast<uint16_t>(p[4] | (p[5] << 8));
            send_y_ = static_cast<uint16_t>(p[6] | (p[7] << 8));
            send_token_ = p[8];
            have_send_ = true;
            std::snprintf(b, sizeof b, "plxa_download index=4 len=%zu magic=%s plxa=%u:%u token=%u", len,
                          magic == kSourceAspectPacketMagic ? "PLXA" : "bad", send_x_, send_y_, send_token_);
        } else {
            std::snprintf(b, sizeof b, "plxa_download index=4 len=%zu (short packet)", len);
        }
        emit(b);
    }
    // sendSourceAspect's outcome: what it sent (the stream's source_dar) and the PLXJ it saw.
    void plxaResult(uint16_t x, uint16_t y, uint8_t token, bool baseline_ok, const SourceAspectAck& baseline,
                    const char* result, const SourceAspectAck& ack) {
        char b[224];
        char base[40];
        if (baseline_ok)
            std::snprintf(base, sizeof base, "%u:%u/%u", baseline.aspect.x, baseline.aspect.y, baseline.token);
        else
            std::snprintf(base, sizeof base, "unread");
        std::snprintf(b, sizeof b, "plxa_send source_dar=%u:%u token=%u baseline=%s result=%s ack=%u:%u/%u", x,
                      y, token, base, result, ack.aspect.x, ack.aspect.y, ack.token);
        std::lock_guard<std::mutex> g(mu_);
        emit(b);
    }
    // The play loop's existing MVPS read: logs a change of session/active/has_frame/error/error_code only.
    void mvps(uint64_t session, bool active, bool has_frame, bool error, uint32_t error_code, uint32_t count,
              uint32_t seq) {
        std::lock_guard<std::mutex> g(mu_);
        if (have_mvps_ && session == mvps_session_ && active == mvps_active_ && has_frame == mvps_has_frame_ &&
            error == mvps_error_ && error_code == mvps_code_)
            return;
        have_mvps_ = true;
        mvps_session_ = session;
        mvps_active_ = active;
        mvps_has_frame_ = has_frame;
        mvps_error_ = error;
        mvps_code_ = error_code;
        char b[192];
        std::snprintf(b, sizeof b, "mvps session=0x%llx active=%d has_frame=%d error=%d code=%u count=%u seq=%u",
                      static_cast<unsigned long long>(session), active, has_frame, error, error_code, count, seq);
        emit(b);
    }

    // ---- arming (ring hooks) ----
    // page: sessionPlxPage's, never 0. The arm line carries the page the poller reads for this session.
    void arm(uint64_t session, uint32_t page, uint32_t build_id) {
        std::lock_guard<std::mutex> g(mu_);
        session_ = session;
        first_sample_ = true;
        read_failed_noted_ = false;
        page_.store(page);
        armed_.store(true);
        char b[160];
        std::snprintf(b, sizeof b, "arm session=0x%llx plx_page=0x%08x source=doorbell build=0x%08X",
                      static_cast<unsigned long long>(session), page, build_id);
        emit(b);
    }
    // No PLX page for this session (sessionPlxPage 0): no arm, and a line that says why. A session still armed
    // is over: its BEGIN has been superseded.
    void armRefused(uint64_t session, uint32_t build_id, const char* why) {
        std::lock_guard<std::mutex> g(mu_);
        disarmLocked("superseded by a BEGIN with no PLX page");
        page_.store(0);
        char b[224];
        std::snprintf(b, sizeof b, "d52 not armed: no PLX page for build 0x%08X (%s) session=0x%llx", build_id,
                      why ? why : "?", static_cast<unsigned long long>(session));
        emit(b);
    }
    uint32_t page() const { return page_.load(); }
    void disarm(const char* why) {
        std::lock_guard<std::mutex> g(mu_);
        disarmLocked(why);
    }
    // The poller's: a disarm for the page it tried, not for a session armed on another page since.
    void disarmPage(uint32_t page, const char* why) {
        std::lock_guard<std::mutex> g(mu_);
        if (page == page_.load())
            disarmLocked(why);
    }

    // ---- poller (one thread) ----
    // A sample read from page: dropped unless it is the armed session's page.
    void sampleFrom(uint32_t page, const PlxWords& w) {
        std::lock_guard<std::mutex> g(mu_);
        if (page == page_.load())
            sampleLocked(w);
    }
    void sample(const PlxWords& w) {
        std::lock_guard<std::mutex> g(mu_);
        sampleLocked(w);
    }
    // The page could not be read (why): one line per arm. The words count as absent, so a page that stays
    // unreadable ends the session's arm through the absent window, as a page with no magic does.
    void readFailed(uint32_t page, const char* why) {
        std::lock_guard<std::mutex> g(mu_);
        if (page != page_.load() || !armed_.load() || read_failed_noted_)
            return;
        read_failed_noted_ = true;
        char b[224];
        std::snprintf(b, sizeof b, "plx read failed page=0x%08x (%s)", page, why ? why : "?");
        emit(b);
    }

    void tick() {
        std::lock_guard<std::mutex> g(mu_);
        const Stamp now = clock_();
        if (hb_last_us_ < 0) {
            hb_last_us_ = now.mono_us;
            return;
        }
        if (now.mono_us - hb_last_us_ < int64_t(cfg_.heartbeat_ms) * 1000)
            return;
        hb_last_us_ = now.mono_us;
        char b[512];
        std::snprintf(b, sizeof b,
                      "hb armed=%d session=0x%llx polls=%llu plxj=%s plxd_frames_done=%s frames=%llu gaps=%llu "
                      "maxgap_ms=%lld stalls=%llu plxf_underrun=%s plxf_debug=%s plxf_live=%d mvps_code=%u "
                      "plxa_downloads=%llu ring_events=%llu has_frame=n/a(spi-only) fb_par=n/a(uio-only) "
                      "visible=n/a r2_out=n/a frame=640x480(const)",
                      armed_.load(), static_cast<unsigned long long>(session_),
                      static_cast<unsigned long long>(polls_), have_j_ ? jText(j_).c_str() : "n/a",
                      have_d_ ? std::to_string(fdOf(d_)).c_str() : "n/a",
                      static_cast<unsigned long long>(hb_frames_), static_cast<unsigned long long>(hb_gaps_),
                      static_cast<long long>(hb_maxgap_us_ / 1000), static_cast<unsigned long long>(hb_stalls_),
                      have_f_ ? std::to_string(underrunOf(f_)).c_str() : "n/a",
                      have_f_ ? hex2(debugOf(f_)).c_str() : "n/a", f_live_ ? 1 : 0, mvps_code_,
                      static_cast<unsigned long long>(sends_), static_cast<unsigned long long>(ring_events_));
        hb_frames_ = hb_gaps_ = hb_stalls_ = 0;
        hb_maxgap_us_ = 0;
        polls_ = 0;
        emit(b);
    }

    static unsigned jx(uint64_t w) { return static_cast<unsigned>((w >> 32) & 0xFFFu); }
    static unsigned jy(uint64_t w) { return static_cast<unsigned>((w >> 44) & 0xFFFu); }
    static unsigned jt(uint64_t w) { return static_cast<unsigned>((w >> 56) & 0xFFu); }
    static unsigned fdOf(uint64_t w) { return static_cast<unsigned>((w >> 48) & 0xFFFFu); }
    static unsigned swapPendingOf(uint64_t w) { return static_cast<unsigned>((w >> 35) & 1u); }
    static unsigned dispBankOf(uint64_t w) { return static_cast<unsigned>((w >> 34) & 1u); }
    static unsigned fseqOf(uint64_t w) { return static_cast<unsigned>((w >> 32) & 0xFFu); }
    static unsigned debugOf(uint64_t w) { return static_cast<unsigned>((w >> 40) & 0xFFu); }
    static unsigned underrunOf(uint64_t w) { return static_cast<unsigned>((w >> 48) & 0xFFFFu); }

private:
    static std::string jText(uint64_t w) {
        return std::to_string(jx(w)) + ":" + std::to_string(jy(w)) + "/" + std::to_string(jt(w));
    }
    static std::string hex2(unsigned v) {
        char b[8];
        std::snprintf(b, sizeof b, "0x%02x", v & 0xFFu);
        return b;
    }

    void emit(const std::string& what) {
        const Stamp s = clock_();
        char head[80];
        std::snprintf(head, sizeof head, "d52 t=%lld.%06lld rt=%lld.%06lld ",
                      static_cast<long long>(s.mono_us / 1000000), static_cast<long long>(s.mono_us % 1000000),
                      static_cast<long long>(s.real_us / 1000000), static_cast<long long>(s.real_us % 1000000));
        sink_(head + what);
    }

    void disarmLocked(const char* why) {
        if (!armed_.load())
            return;
        armed_.store(false);
        emit(std::string("disarm (") + why + ")");
        have_j_ = have_d_ = have_f_ = false;
        gap_open_reported_ = false;
        absent_since_us_ = -1;
    }

    void sampleLocked(const PlxWords& w) {
        if (!armed_.load())
            return;
        const Stamp now = clock_();
        ++polls_;
        const bool any = w.j_ok || w.d_ok || w.f_ok;
        if (!any) {
            if (absent_since_us_ < 0)
                absent_since_us_ = now.mono_us;
            else if (now.mono_us - absent_since_us_ >= int64_t(cfg_.absent_ms) * 1000) {
                disarmLocked("PLX page absent (no PLXJ/PLXD/PLXF magic): the Plex core is gone");
                return;
            }
        } else {
            absent_since_us_ = -1;
        }
        if (first_sample_) {
            first_sample_ = false;
            baselineLocked(w, now);
            return;
        }
        plxjLocked(w);
        plxfLocked(w, now);
        plxdLocked(w, now);
    }

    void baselineLocked(const PlxWords& w, const Stamp& now) {
        have_j_ = w.j_ok;
        j_ = w.j;
        have_d_ = w.d_ok;
        d_ = w.d;
        have_f_ = w.f_ok;
        f_ = w.f;
        f_logged_ = w.f;
        last_advance_us_ = now.mono_us;
        f_seq_us_ = now.mono_us;
        f_live_ = w.f_ok;
        gap_open_reported_ = false;
        char b[256];
        std::snprintf(b, sizeof b,
                      "plx baseline plxj=%s plxd frames_done=%s swap_pending=%s disp_bank=%s plxf underrun=%s "
                      "debug=%s",
                      w.j_ok ? jText(w.j).c_str() : "absent", w.d_ok ? std::to_string(fdOf(w.d)).c_str() : "absent",
                      w.d_ok ? std::to_string(swapPendingOf(w.d)).c_str() : "-",
                      w.d_ok ? std::to_string(dispBankOf(w.d)).c_str() : "-",
                      w.f_ok ? std::to_string(underrunOf(w.f)).c_str() : "absent",
                      w.f_ok ? hex2(debugOf(w.f)).c_str() : "-");
        emit(b);
    }

    void plxjLocked(const PlxWords& w) {
        if (w.j_ok != have_j_) {
            emit(w.j_ok ? "plxj present " + jText(w.j) : std::string("plxj absent (magic/unstable)"));
            have_j_ = w.j_ok;
            j_ = w.j;
            return;
        }
        if (!w.j_ok || w.j == j_)
            return;
        // review 17:22: token = the daemon's logged send -> daemon; = last+1 with no logged send -> an unlogged
        // daemon send; any other token -> a stray index-4 download or corruption.
        const char* src;
        if (have_send_ && jt(w.j) == send_token_ && jx(w.j) == send_x_ && jy(w.j) == send_y_)
            src = "logged_plxa_download";
        else if (jt(w.j) == ((jt(j_) + 1u) & 0xFFu))
            src = "token_last+1_NO_logged_download";
        else
            src = "other_token_NO_logged_download";
        emit("plxj change " + jText(j_) + " -> " + jText(w.j) + " src=" + src);
        j_ = w.j;
    }

    void plxfLocked(const PlxWords& w, const Stamp& now) {
        if (w.f_ok != have_f_) {
            emit(w.f_ok ? std::string("plxf present") : std::string("plxf absent (magic/unstable)"));
            have_f_ = w.f_ok;
            f_ = w.f;
            f_seq_us_ = now.mono_us;
            return;
        }
        if (!w.f_ok)
            return;
        if (fseqOf(w.f) != fseqOf(f_)) {
            f_seq_us_ = now.mono_us;
            if (!f_live_) {
                f_live_ = true;
                emit("plxf live (seq advancing again)");
            }
        } else if (f_live_ && now.mono_us - f_seq_us_ >= int64_t(cfg_.stall_ms) * 1000) {
            f_live_ = false;
            emit("plxf stale (seq unchanged " + std::to_string((now.mono_us - f_seq_us_) / 1000) + " ms)");
        }
        // Against the last LOGGED state: a debug change always logs; underrun changes at most once a second, and a
        // trailing change still logs once the second has passed.
        const bool debugChanged = debugOf(w.f) != debugOf(f_logged_);
        const bool underrunChanged = underrunOf(w.f) != underrunOf(f_logged_);
        if (debugChanged || (underrunChanged && now.mono_us - f_line_us_ >= 1000000)) {
            emit("plxf underrun " + std::to_string(underrunOf(f_logged_)) + " -> " + std::to_string(underrunOf(w.f)) +
                 " debug " + hex2(debugOf(f_logged_)) + " -> " + hex2(debugOf(w.f)));
            f_line_us_ = now.mono_us;
            f_logged_ = w.f;
        }
        f_ = w.f;
    }

    void plxdLocked(const PlxWords& w, const Stamp& now) {
        if (w.d_ok != have_d_) {
            emit(w.d_ok ? std::string("plxd present") : std::string("plxd absent (magic/unstable)"));
            have_d_ = w.d_ok;
            d_ = w.d;
            last_advance_us_ = now.mono_us;
            gap_open_reported_ = false;
            return;
        }
        if (!w.d_ok)
            return;
        const unsigned a = fdOf(d_), b = fdOf(w.d);
        const int64_t gap = now.mono_us - last_advance_us_;
        if (a != b) {
            const unsigned delta = (b - a) & 0xFFFFu;
            if (delta > 0x8000u) {
                emit("plxd frames_done jump " + std::to_string(a) + " -> " + std::to_string(b));
            } else {
                hb_frames_ += delta;
                if (gap > hb_maxgap_us_)
                    hb_maxgap_us_ = gap;
                if (gap >= int64_t(cfg_.gap_ms) * 1000) {
                    ++hb_gaps_;
                    char m[192];
                    std::snprintf(m, sizeof m,
                                  "plxd gap ms=%lld frames_done %u -> %u swap_pending=%u disp_bank=%u since_t=%lld.%06lld",
                                  static_cast<long long>(gap / 1000), a, b, swapPendingOf(w.d), dispBankOf(w.d),
                                  static_cast<long long>(last_advance_us_ / 1000000),
                                  static_cast<long long>(last_advance_us_ % 1000000));
                    emit(m);
                }
            }
            last_advance_us_ = now.mono_us;
            gap_open_reported_ = false;
        } else if (!gap_open_reported_ && gap >= int64_t(cfg_.stall_ms) * 1000) {
            gap_open_reported_ = true;
            ++hb_stalls_;
            char m[160];
            std::snprintf(m, sizeof m, "plxd stall ms=%lld frames_done=%u swap_pending=%u disp_bank=%u",
                          static_cast<long long>(gap / 1000), b, swapPendingOf(w.d), dispBankOf(w.d));
            emit(m);
        }
        d_ = w.d;
    }

    Config cfg_;
    Sink sink_;
    Clock clock_;
    std::mutex mu_;
    std::atomic<bool> armed_{false};
    std::atomic<uint32_t> page_{0}; // the armed session's PLX page (sessionPlxPage), 0 when none
    uint64_t session_ = 0;
    bool first_sample_ = true;
    bool have_j_ = false, have_d_ = false, have_f_ = false;
    uint64_t j_ = 0, d_ = 0, f_ = 0, f_logged_ = 0;
    int64_t last_advance_us_ = 0, f_seq_us_ = 0, f_line_us_ = -1000000000, absent_since_us_ = -1;
    bool f_live_ = false, gap_open_reported_ = false;
    bool read_failed_noted_ = false;
    bool have_send_ = false;
    uint16_t send_x_ = 0, send_y_ = 0;
    uint8_t send_token_ = 0;
    bool have_mvps_ = false, mvps_active_ = false, mvps_has_frame_ = false, mvps_error_ = false;
    uint64_t mvps_session_ = 0;
    uint32_t mvps_code_ = 0;
    int64_t hb_last_us_ = -1, hb_maxgap_us_ = 0;
    uint64_t polls_ = 0, hb_frames_ = 0, hb_gaps_ = 0, hb_stalls_ = 0, sends_ = 0, ring_events_ = 0;
};

// How one PLX word load went (PlxPage::Reader).
enum class PlxRead { Ok, OtherPage, Unavailable };

// The PLX page reads (review 08:17 (b), 08:18): D52-LOG has no map and no fd of its own. Every load goes
// through the reader the daemon attaches at each arm: FpgaSpi::plxPageRead32 on the daemon's one mapper of
// the page, under that mapper's lock. Both reads of a word must agree and carry the word's magic; nothing is
// ever written. detach() waits for a read in flight, so the reader's owner may go once it returns.
class PlxPage {
public:
    using Reader = std::function<PlxRead(uint32_t page, uint32_t off, uint32_t& out)>;

    PlxPage() = default;
    PlxPage(const PlxPage&) = delete;
    PlxPage& operator=(const PlxPage&) = delete;
#ifdef MUT_D52_OWN_MMAP
    ~PlxPage() {
        if (mut_map_)
            ::munmap(const_cast<uint8_t*>(mut_map_), 4096);
        if (mut_fd_ >= 0)
            ::close(mut_fd_);
    }
#endif

    void attach(const void* owner, Reader r) {
        std::lock_guard<std::mutex> g(mu_);
        owner_ = owner;
        reader_ = std::move(r);
    }
    // Only the attached owner's detach drops the reader.
    void detach(const void* owner) {
        std::lock_guard<std::mutex> g(mu_);
        if (owner_ != owner)
            return;
        owner_ = nullptr;
        reader_ = nullptr;
    }

    // st: Ok, or the first failure (the words read before it stand; the rest are not ok).
    PlxWords read(uint32_t page, PlxRead& st) {
        std::lock_guard<std::mutex> g(mu_);
        PlxWords w;
        st = reader_ ? PlxRead::Ok : PlxRead::Unavailable;
        if (st == PlxRead::Ok)
            st = word(page, kPlxjOff, kPlxjMagic, w.j_ok, w.j);
        if (st == PlxRead::Ok)
            st = word(page, kPlxdOff, kPlxdMagic, w.d_ok, w.d);
        if (st == PlxRead::Ok)
            st = word(page, kPlxfOff, kPlxfMagic, w.f_ok, w.f);
        return w;
    }

private:
    // Caller holds mu_.
    PlxRead load(uint32_t page, uint32_t off, uint32_t& v) {
#ifdef MUT_D52_OWN_MMAP   // test-only mutant (review 08:17 (b)): the pre-08:17 own read-only map of the page
        if (!mut_map_) {
            mut_fd_ = ::open(MPX_D52_DEVMEM_PATH, O_RDONLY | O_SYNC | O_CLOEXEC);
            void* p = mut_fd_ < 0 ? MAP_FAILED
                                  : ::mmap(nullptr, 4096, PROT_READ, MAP_SHARED, mut_fd_, static_cast<off_t>(page));
            if (p != MAP_FAILED)
                mut_map_ = static_cast<const volatile uint8_t*>(p);
        }
        if (mut_map_) {
            v = *reinterpret_cast<const volatile uint32_t*>(mut_map_ + off);
            return PlxRead::Ok;
        }
#endif
        return reader_(page, off, v);
    }

    PlxRead word(uint32_t page, uint32_t off, uint32_t magic, bool& ok, uint64_t& out) {
        uint32_t lo0 = 0, hi0 = 0, lo1 = 0, hi1 = 0;
        PlxRead r = load(page, off, lo0);
        if (r == PlxRead::Ok)
            r = load(page, off + 4, hi0);
        __sync_synchronize();
        if (r == PlxRead::Ok)
            r = load(page, off, lo1);
        if (r == PlxRead::Ok)
            r = load(page, off + 4, hi1);
        if (r != PlxRead::Ok)
            return r;
        ok = lo0 == lo1 && hi0 == hi1 && lo0 == magic;
        if (ok)
            out = static_cast<uint64_t>(lo0) | (static_cast<uint64_t>(hi0) << 32);
        return PlxRead::Ok;
    }

    std::mutex mu_;
    const void* owner_ = nullptr;
    Reader reader_;
#ifdef MUT_D52_OWN_MMAP
    int mut_fd_ = -1;
    const volatile uint8_t* mut_map_ = nullptr;
#endif
};

// The poller thread: samples while armed, heartbeats always. Started once, on the first arm. It reads the armed
// session's page (Logger::page) through the attached reader and drops a sample of any other page.
class Poller {
public:
    explicit Poller(Logger& l) : log_(l) {}
    ~Poller() { stop(); }

    void attach(const void* owner, PlxPage::Reader r) { page_.attach(owner, std::move(r)); }
    void detach(const void* owner) { page_.detach(owner); }

    void ensureStarted() {
        std::lock_guard<std::mutex> g(mu_);
        if (thread_.joinable())
            return;
        thread_ = std::thread([this] { run(); });
    }
    void stop() {
        {
            std::lock_guard<std::mutex> g(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }

private:
    void run() {
        std::unique_lock<std::mutex> lk(mu_);
        while (!stop_) {
            lk.unlock();
            if (log_.armed()) {
                const uint32_t want = log_.page();
                if (want) {
                    PlxRead st = PlxRead::Ok;
                    const PlxWords w = page_.read(want, st);
                    if (st == PlxRead::OtherPage)
                        log_.readFailed(want, "not the daemon's doorbell page; D52-LOG reads only that page, "
                                              "through the daemon's one mapper");
                    else if (st != PlxRead::Ok)
                        log_.readFailed(want, "the daemon's mapper gave no read: no reader, the core gate is "
                                              "closed or the map failed");
                    log_.sampleFrom(want, w);
                }
            }
            log_.tick();
            lk.lock();
            cv_.wait_for(lk, std::chrono::milliseconds(log_.config().poll_ms), [this] { return stop_; });
        }
    }

    Logger& log_;
    PlxPage page_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

// The process-wide instance: nullptr unless MPX_D52_LOG=1, so every hook is one branch when off.
// Intentionally never destroyed (the poller thread lives until the process exits).
struct Instance {
    Logger logger;
    Poller poller;
    explicit Instance(const Config& c) : logger(c), poller(logger) {}
};

// Set once the instance exists; detachReader reads it without creating the instance (or reading the env).
inline std::atomic<Instance*>& liveInstance() {
    static std::atomic<Instance*> live{nullptr};
    return live;
}

inline Instance* instance() {
    static Instance* inst = []() -> Instance* {
        const Config c = configFromEnv();
        if (!c.enabled)
            return nullptr;
        auto* i = new Instance(c);
        i->logger.start(static_cast<int>(::getpid()));
        liveInstance().store(i);
        return i;
    }();
    return inst;
}

inline Logger* active() {
    Instance* i = instance();
    return i ? &i->logger : nullptr;
}

// BEGIN acknowledged: arm the poller on the session's PLX page (starting its thread the first time). build_id:
// the session's CAPS build; doorbell_phys / phys_base: the daemon's resolved DDR layout (FpgaSpi ddrLayout_).
// owner / reader: the daemon's one mapper of the page (FpgaSpi), the poller's only way to read it.
// No page (sessionPlxPage 0): no arm, one 'd52 not armed: no PLX page for build 0x...' line.
inline void armSession(uint64_t session, uint32_t build_id, uint32_t doorbell_phys, uint32_t phys_base,
                       const void* owner, PlxPage::Reader reader) {
    if (Instance* i = instance()) {
        const char* why = nullptr;
        const uint32_t page = sessionPlxPage(build_id, doorbell_phys, phys_base, &why);
        if (!page) {
            i->logger.armRefused(session, build_id, why);
            return;
        }
        i->poller.attach(owner, std::move(reader));
        i->logger.arm(session, page, build_id);
        i->poller.ensureStarted();
    }
}

// The reader's owner is going (FpgaSpi::close): no read through it starts after this returns, and one in
// flight has finished. A no-op when D52-LOG is off (the instance is never created here).
inline void detachReader(const void* owner) {
    if (Instance* i = liveInstance().load())
        i->poller.detach(owner);
}

inline const char* eventName(int e) {
    switch (e) {
    case 1: return "BEGIN";
    case 2: return "NAL";
    case 3: return "FLUSH";
    case 4: return "END";
    case 5: return "PAUSE";
    case 6: return "RESUME";
    case 7: return "PROBE";
    case 8: return "AU";
    case 9: return "DRAIN";
    default: return "EVENT?";
    }
}

} // namespace d52
} // namespace misterplex
