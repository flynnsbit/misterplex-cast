// MPX OwenbRing: see owenb_ring.hpp. The library is not thread-safe, so every call into it is
// made under mu_.
#include "owenb_ring.hpp"

#include "h264ring_cavlc.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace misterplex::owenb {

using h264stream::ControlResult;
using h264stream::PushResult;

const char* toString(ProbeVerdict v) {
    switch (v) {
    case ProbeVerdict::NoMap: return "NoMap";
    case ProbeVerdict::NoCore: return "NoCore";
    case ProbeVerdict::BadVersion: return "BadVersion";
    case ProbeVerdict::BadGeometry: return "BadGeometry";
    case ProbeVerdict::Stale: return "Stale";
    case ProbeVerdict::Alive: return "Alive";
    }
    return "?";
}

int64_t toPts90k(int64_t ts, uint32_t num, uint32_t den) {
    if (ts < 0 || num == 0 || den == 0)
        return h264stream::kNoPts90k;
    // ts * num * 90000 / den, mod 2^33, exact in 64-bit arithmetic (armhf has no __int128).
    // Unsigned wrap keeps the low bits, and the low bits are all the result needs.
    constexpr uint64_t kMask = (1ull << 33) - 1;
    const uint64_t t = static_cast<uint64_t>(ts);
    const uint64_t q = static_cast<uint64_t>(num) * 90000u;   // < 2^49
    const uint64_t whole = t / den, rem = t % den;            // rem < den < 2^32
    const uint64_t a = whole * q;                             // whole units of den: exact mod 2^64
    const uint64_t b = rem * (q / den) + rem * (q % den) / den;   // rem * (q % den) < 2^64
    return static_cast<int64_t>((a + b) & kMask);
}

namespace {
void beBarrier(void* u) { static_cast<OwenbDdrWindow*>(u)->barrier(); }
void beSleep(void* u, unsigned us) { static_cast<OwenbDdrWindow*>(u)->sleepUs(us); }
uint64_t beNow(void* u) { return static_cast<OwenbDdrWindow*>(u)->nowUs(); }

// Mutation hook for the mutant-check rule. Normal builds never define it.
#ifdef MUT_OWENB_SKIP_HEARTBEAT
constexpr bool kCheckHeartbeat = false;
#else
constexpr bool kCheckHeartbeat = true;
#endif
}  // namespace

OwenbRing::OwenbRing(OwenbDdrWindow& window, unsigned timeout_ms)
    : window_(window), timeout_ms_(timeout_ms) {}

OwenbRing::~OwenbRing() {
    std::lock_guard<std::mutex> g(mu_);
    if (ring_)
        h264ring_close(ring_);   // the window owns the mapping; close only frees the handle
    ring_ = nullptr;
}

void OwenbRing::fail(int rc, const char* what) {
    last_errno_ = rc;
    last_error_ = std::string(what) + ": " + std::strerror(rc < 0 ? -rc : rc);
}

RingProbe OwenbRing::probe(unsigned heartbeat_ms) {
    std::lock_guard<std::mutex> g(mu_);
    RingProbe p;
    if (!window_.mapped())
        return p;   // NoMap
    volatile uint64_t* c = window_.ctrl();
    p.magic = c[0];
    p.version = static_cast<uint32_t>(c[1] & 0xFFFFu);
    p.ring_size = static_cast<uint32_t>(c[2] >> 32);
    p.heartbeat = static_cast<uint32_t>(c[3]);
    if (!ring_) {
        h264ring_backend be;
        std::memset(&be, 0, sizeof be);
        be.ctrl = window_.ctrl();
        be.ring = window_.ring();
        be.ring_size = window_.ringSize();
        be.barrier = beBarrier;
        be.sleep_us = beSleep;
        be.now_us = beNow;
        be.user = &window_;
        const int rc = h264ring_attach(&ring_, &be);
        if (rc == -ENODEV) {
            p.verdict = ProbeVerdict::NoCore;
            return p;
        }
        if (rc == -EPROTONOSUPPORT) {
            p.verdict = h264ring_version_ok(p.version, H264RING_VERSION) ? ProbeVerdict::BadGeometry
                                                                         : ProbeVerdict::BadVersion;
            fail(rc, toString(p.verdict));
            return p;
        }
        if (rc) {
            fail(rc, "h264ring_attach");
            return p;   // NoMap
        }
        h264ring_set_rewrite(ring_, rewrite_ ? 1 : 0);
    }
    if (kCheckHeartbeat && !active_) {
        const int rc = h264ring_alive(ring_, heartbeat_ms);
        if (rc) {
            fail(rc, "h264ring_alive");
            h264ring_close(ring_);
            ring_ = nullptr;
            p.verdict = rc == -ENODEV ? ProbeVerdict::NoCore : ProbeVerdict::Stale;
            return p;
        }
    }
    p.heartbeat = static_cast<uint32_t>(c[3]);
    p.verdict = ProbeVerdict::Alive;
    return p;
}

ControlResult OwenbRing::reinitOutput() {
    if (outputReinited_)
        return ControlResult::Ok;
    // The next session start is the one session. new_session already resets
    // the decoder, queued pictures, and display, which is what a previous
    // daemon's picture needs. A throwaway start plus flush here opened two
    // sessions before the idle card's own start, so the ready card counted three.
    outputReinited_ = true;
    return ControlResult::Ok;
}

ControlResult OwenbRing::begin(uint64_t session_id) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (active_)
            return ControlResult::ActiveSession;
    }
    if (probe(timeout_ms_).verdict != ProbeVerdict::Alive)
        return ControlResult::Fatal;
    std::lock_guard<std::mutex> g(mu_);
    const int rc = h264ring_start_session(ring_, timeout_ms_);
    if (rc) {
        fail(rc, "h264ring_start_session");
        return ControlResult::Fatal;
    }
    h264ring_set_pause(ring_, 0);   // never inherit a previous session's pause
    session_id_ = session_id;
    producer_seq_ = consumer_seq_ = 0;
    bytes_accepted_ = nal_accepted_ = 0;
    desync_count_ = 0;
    last_bad_seq_ = 0;
    active_ = true;
    paused_ = false;
    return ControlResult::Ok;
}

PushResult OwenbRing::mapWrite(int rc, const char* what) {
    if (rc == 0)
        return PushResult::Ok;
    if (rc == -EAGAIN) {
        ++overrun_count_;
        return PushResult::Full;
    }
    fail(rc, what);
    if (rc == -ENOTCONN || rc == -EPROTO)
        active_ = false;   // an OSD Reset or another program took the core, or the core is in ERROR
    return PushResult::Fatal;
}

PushResult OwenbRing::pushNal(const h264stream::NalView& nal) {
    h264stream::AccessUnitView au;
    au.session_id = nal.session_id;
    au.seq = nal.seq;
    au.annexb = nal.annexb;
    au.len = nal.len;
    return pushAccessUnit(au);
}

PushResult OwenbRing::pushAccessUnit(const h264stream::AccessUnitView& au) {
    std::lock_guard<std::mutex> g(mu_);
    if (!active_ || au.session_id != session_id_ || !au.annexb || au.len == 0 ||
        au.len > H264RING_MAX_PAYLOAD)
        return PushResult::Fatal;
    if (au.seq != producer_seq_) {
        ++desync_count_;
        last_bad_seq_ = au.seq;
        return PushResult::Desync;
    }
    unsigned flags = 0;
    if (au.discontinuity) flags |= H264RING_F_DISCONTINUITY;
    if (au.end_of_stream) flags |= H264RING_F_END_OF_STREAM;
    if (au.low_latency) flags |= H264RING_F_LOW_LATENCY;
    // The library copies the AU into the ring before it returns (copy-on-push holds).
    const PushResult r = mapWrite(
        h264ring_try_write_au(ring_, au.annexb, static_cast<uint32_t>(au.len), au.pts90k, flags),
        "h264ring_try_write_au");
    if (r == PushResult::Ok) {
        ++producer_seq_;
        bytes_accepted_ += au.len;
        ++nal_accepted_;
    }
    return r;
}

PushResult OwenbRing::pushPcm(const h264stream::PcmView& pcm) {
    std::lock_guard<std::mutex> g(mu_);
    if (!active_ || pcm.session_id != session_id_ || !pcm.s16le || pcm.len > H264RING_MAX_PAYLOAD)
        return PushResult::Fatal;
    // The library rejects a zero length or one that is not a multiple of 4 with -EINVAL.
    return mapWrite(h264ring_try_write_audio(ring_, pcm.s16le, static_cast<uint32_t>(pcm.len),
                                             pcm.pts90k),
                    "h264ring_try_write_audio");
}

ControlResult OwenbRing::flush(uint64_t session_id) {
    std::lock_guard<std::mutex> g(mu_);
    if (!active_ || session_id != session_id_)
        return ControlResult::NoSession;
    const int rc = h264ring_flush(ring_, timeout_ms_);
    if (rc) {
        fail(rc, "h264ring_flush");
        return ControlResult::Fatal;
    }
    consumer_seq_ = producer_seq_;
    return ControlResult::Ok;
}

ControlResult OwenbRing::end(uint64_t session_id) {
    std::lock_guard<std::mutex> g(mu_);
    if (!active_ || session_id != session_id_)
        return ControlResult::NoSession;
    if (paused_)
        h264ring_set_pause(ring_, 0);   // let the queued tail drain; the last picture stays
    active_ = false;
    paused_ = false;
    return ControlResult::Ok;
}

ControlResult OwenbRing::pause(uint64_t session_id) {
    std::lock_guard<std::mutex> g(mu_);
    if (!active_ || session_id != session_id_)
        return ControlResult::NoSession;
    h264ring_set_pause(ring_, 1);
    paused_ = true;
    return ControlResult::Ok;
}

ControlResult OwenbRing::resume(uint64_t session_id) {
    std::lock_guard<std::mutex> g(mu_);
    if (!active_ || session_id != session_id_)
        return ControlResult::NoSession;
    h264ring_set_pause(ring_, 0);
    paused_ = false;
    return ControlResult::Ok;
}

ControlResult OwenbRing::setAttenuation(uint64_t session_id, unsigned half_db) {
    std::lock_guard<std::mutex> g(mu_);
    if (!active_ || session_id != session_id_)
        return ControlResult::NoSession;
    const int rc = h264ring_set_atten(ring_, half_db);
    if (rc) {
        fail(rc, "h264ring_set_atten");
        return ControlResult::Fatal;
    }
    return ControlResult::Ok;
}

ControlResult OwenbRing::release(uint64_t session_id) {
    std::lock_guard<std::mutex> g(mu_);
    if (!active_ || session_id != session_id_)
        return ControlResult::NoSession;
    active_ = false;
    paused_ = false;
    return ControlResult::Ok;
}

CoreStatus OwenbRing::coreStatus() const {
    std::lock_guard<std::mutex> g(mu_);
    CoreStatus cs;
    if (!ring_)
        return cs;
    struct ::h264ring_status st;
    if (h264ring_status(ring_, &st) != 0)
        return cs;
    cs.valid = true;
    cs.state = static_cast<unsigned>(H264RING_ST(&st, STATE));
    cs.err = static_cast<unsigned>(H264RING_ST(&st, ERR));
    cs.live = static_cast<unsigned>(H264RING_ST(&st, LIVE));
    if (cs.live & H264RING_L_STC_RUN)
        cs.stc90k = static_cast<int64_t>(H264RING_ST(&st, STC));
    cs.sessionOk = h264ring_session_ok(ring_) != 0;
    cs.ringLevel = window_.ringSize() - h264ring_free(ring_);
    cs.refusedCode = static_cast<unsigned>(H264RING_ST(&st, FE_REFUSED_CODE));
    return cs;
}

int OwenbRing::overlayShow(const struct ::h264ring_overlay& o) {
    std::lock_guard<std::mutex> g(mu_);
    if (!ring_)
        return -ENOTCONN;
    return h264ring_overlay_show(ring_, &o);
}

int OwenbRing::overlayHide() {
    std::lock_guard<std::mutex> g(mu_);
    if (!ring_)
        return -ENOTCONN;
    return h264ring_overlay_hide(ring_);
}

OverlayCoreStatus OwenbRing::overlayStatus() const {
    std::lock_guard<std::mutex> g(mu_);
    OverlayCoreStatus os;
    if (!ring_)
        return os;
    struct ::h264ring_status st;
    if (h264ring_status(ring_, &st) != 0)
        return os;
    os.valid = true;
    os.gen = static_cast<uint32_t>(H264RING_ST(&st, OVL_GEN));
    os.state = static_cast<unsigned>(H264RING_ST(&st, OVL_STATE));
    os.err = static_cast<unsigned>(H264RING_ST(&st, OVL_ERR));
    os.at = H264RING_ST(&st, OVL_AT) != 0;
    h264ring_raster(&st, &os.raster);
    os.videoOut = h264ring_video_out(&st);
    os.osdAnalogAspect = static_cast<unsigned>(H264RING_ST(&st, OSD_ANALOG_ASPECT));
    os.osdAspect = static_cast<unsigned>(H264RING_ST(&st, OSD_ASPECT));
    os.setSeq = static_cast<unsigned>(H264RING_ST(&st, SET_SEQ));
    os.setErr = static_cast<unsigned>(H264RING_ST(&st, SET_ERR));
    // the core still runs this handle's session (after end()/release() too: the overlay and the
    // settings still work until another program's session or an OSD Reset, h264ring.h:104-115)
    os.session = h264ring_session_ok(ring_) != 0;
    return os;
}

int OwenbRing::input(struct ::h264ring_input& in) const {
    std::lock_guard<std::mutex> g(mu_);
    if (!ring_)
        return -ENODEV;
    return h264ring_input(ring_, &in);
}

int OwenbRing::requestSettings(unsigned mask, const unsigned values[H264RING_OPT_COUNT]) {
    std::lock_guard<std::mutex> g(mu_);
    if (!ring_)
        return -ENOTCONN;
    return h264ring_request_settings(ring_, mask, values);
}

int OwenbRing::clearSettings() {
    std::lock_guard<std::mutex> g(mu_);
    if (!ring_)
        return -ENOTCONN;
    return h264ring_clear_settings(ring_);
}

std::string OwenbRing::rewriteWhy() const {
    std::lock_guard<std::mutex> g(mu_);
    if (!ring_)
        return {};
    const char* w = h264ring_rewrite_why(ring_);
    return w ? w : "";
}

h264stream::Telemetry OwenbRing::status() const {
    std::lock_guard<std::mutex> g(mu_);
    h264stream::Telemetry t;
    t.session_id = session_id_;
    t.ring_capacity_bytes = window_.ringSize();
    if (ring_)
        t.ring_level_bytes = window_.ringSize() - h264ring_free(ring_);
    t.producer_seq = producer_seq_;
    t.consumer_seq = consumer_seq_;
    t.bytes_accepted = bytes_accepted_;
    t.nal_accepted = nal_accepted_;
    t.overrun_count = overrun_count_;
    t.desync_count = desync_count_;
    t.last_bad_seq = last_bad_seq_;
    t.active = active_;
    t.paused = paused_;
    return t;
}

void OwenbRing::setCavlcRewrite(bool on) {
    std::lock_guard<std::mutex> g(mu_);
    rewrite_ = on;
    if (ring_)
        h264ring_set_rewrite(ring_, on ? 1 : 0);
}

int OwenbRing::lastErrno() const {
    std::lock_guard<std::mutex> g(mu_);
    return last_errno_;
}

std::string OwenbRing::lastError() const {
    std::lock_guard<std::mutex> g(mu_);
    return last_error_;
}

}  // namespace misterplex::owenb
