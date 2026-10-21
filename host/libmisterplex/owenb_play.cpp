// OWENB-P1B T5: the daemon's OB play worker (owenb_play.hpp). T7: the long-lived idle session.
#include "owenb_play.hpp"

#include "owenb_aspect.hpp"

#include <cerrno>
#include <chrono>
#include <memory>
#include <utility>

namespace misterplex::owenb {

namespace {

// 0008: reads a play's input unchanged and, from its first video keyframe (the SPS rides in band
// after h264_mp4toannexb), tells the aspect guard whether this is a VUI-less 4:3 stream
// (owenb_aspect.hpp owenbStreamAspect). Gives up (known, no override) after kGiveUp video packets.
class AspectSniffSource final : public IMediaSource {
public:
    static constexpr unsigned kGiveUp = 30;
    AspectSniffSource(std::unique_ptr<IMediaSource> in, owenb_osd::AspectGuard& g, std::function<void(const std::string&)> log)
        : in_(std::move(in)), g_(g), log_(std::move(log)) {}
    SourceRead read(MediaPacket& p, std::string& err) override {
        const SourceRead r = in_->read(p, err);
        if (r != SourceRead::Packet || done_ || p.kind != MediaPacket::Kind::Video)
            return r;
        int64_t cn = 0, cd = 0;
        const bool hc = in_->containerAspect(cn, cd);
        const OwenbStreamAspect a = owenbStreamAspect(p.data.data(), p.data.size(), hc, cn, cd);
        if (!a.spsFound && !p.keyframe && ++video_ < kGiveUp)
            return r;
        done_ = true;
        if (g_.stream().known)   // an earlier input of this play (before a seek) decided
            return r;
        owenb_osd::StreamAspect sa;
        sa.known = true;
        sa.force43 = a.force43;
        sa.square43 = a.square43;
        g_.setStream(sa);
        if (log_ && a.spsFound && !a.spsAspect)
            log_("aspect: the stream's SPS has no aspect (the core reads 16:9); display " + std::to_string(a.darNum) +
                 ":" + std::to_string(a.darDen) + (hc ? " (container)" : " (picture)") +
                 (a.force43 ? (a.square43 ? " -> a 4:3 candidate (square samples; the guard decides by Video Output)"
                                         : " -> a 4:3 candidate (the guard decides)")
                            : ""));
        return r;
    }
    void cancel() override { in_->cancel(); }
    bool containerAspect(int64_t& num, int64_t& den) const override { return in_->containerAspect(num, den); }

private:
    std::unique_ptr<IMediaSource> in_;
    owenb_osd::AspectGuard& g_;
    std::function<void(const std::string&)> log_;
    bool done_ = false;
    unsigned video_ = 0;
};

} // namespace

OwenbPlayer::OwenbPlayer(WindowFactory window, SourceFactory source)
    : windowFactory_(std::move(window)), sourceFactory_(std::move(source)) {}

OwenbPlayer::~OwenbPlayer() {
    stopIdle();
    stop();
}

void OwenbPlayer::logLocked(const std::string& s) const {
    if (log_)
        log_(s);
}

// The window, the handle and its port (once; the idle session and every play share them).
bool OwenbPlayer::openRingLocked(std::string& err) {
    if (ring_)
        return true;
    window_ = windowFactory_ ? windowFactory_(err) : nullptr;
    if (!window_ || !window_->mapped()) {
        if (err.empty())
            err = "the owenb DDR window did not open";
        window_.reset();
        return false;
    }
    ring_ = std::make_unique<OwenbRing>(*window_, 250);
    const RingProbe p = ring_->probe(100);
    if (p.verdict != ProbeVerdict::Alive) {
        err = std::string("the PlexOB core is not alive on the ring (") + toString(p.verdict) + ")";
        ring_.reset();
        window_.reset();
        return false;
    }
    port_ = std::make_unique<RingOverlayPort>(*ring_);   // the overlay and the aspect guard's port
    // Hot-swap: a killed daemon can leave the core's decoder and display
    // holding the previous picture. Mark the handle once; the next begin()
    // is the session that resets that state.
    const h264stream::ControlResult reinited = ring_->reinitOutput();
    if (reinited != h264stream::ControlResult::Ok) {
        err = "the core output did not reinit (" + ring_->lastError() + ")";
        port_.reset();
        ring_.reset();
        window_.reset();
        return false;
    }
    logLocked("ob: reinit core output");
    return true;
}

// fb0 and the overlay thread (once; it lives across the idle session and the plays).
void OwenbPlayer::startOsdLocked() {
    if (osd_ || !osdCfg_.enabled || !osdCfg_.mapFb || !port_)
        return;
    owenb_osd::FbMap fb;
    std::string ferr;
    if (!osdCfg_.mapFb(fb, ferr) || !fb.virt || !fb.len) {
        logLocked("overlay: fb0 not mapped (" + (ferr.empty() ? std::string("no framebuffer") : ferr) + "): no OSD");
        return;
    }
    fbMapped_ = true;
    std::function<int64_t()> clock = osdCfg_.clockMs;
    if (!clock)
        clock = [] {
            return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count());
        };
    osd_ = std::make_unique<owenb_osd::OsdThread>(ui_, *port_, fb, osdCfg_.allowDouble, log_, clock);
    if (osdCfg_.onSurfaceShown)
        osd_->setOnSurfaceShown(osdCfg_.onSurfaceShown);
    osd_->start();
}

// Everything off: the overlay hidden (while the handle is open), fb0 unmapped, the handle closed
// (h264ring_close: the lock and the rumble go, h264ring.h:94), the window unmapped.
void OwenbPlayer::teardownLocked() {
    if (osd_) {
        osd_->stop();
        osdLast_ = osd_->stats();
        osd_.reset();
    }
    if (fbMapped_ && osdCfg_.unmapFb)
        osdCfg_.unmapFb();
    fbMapped_ = false;
    session_.reset();
    port_.reset();
    ring_.reset();
    window_.reset();   // DevMemDdrWindow: unmaps and drops the ring flock
    idleUp_ = false;
}

// T7: a session with no stream: the core shows black / its idle bars, and our overlay page on top
// (the overlay is shown only during a session, h264ring.h:286). begin() + release(): the core keeps
// running this handle's session; a play's begin() takes over with a new nonce.
bool OwenbPlayer::idleBeginLocked() {
    const h264stream::ControlResult rc = ring_->begin(kIdleSessionId);
    if (rc != h264stream::ControlResult::Ok) {
        if (!idleFailLogged_)
            logLocked("ob idle: the idle session did not start (" + ring_->lastError() + "); retrying");
        idleFailLogged_ = true;
        return false;
    }
    ring_->release(kIdleSessionId);
    idleFailLogged_ = false;
    idleUp_ = true;
    ++idleStarts_;
    aspect_.newSession();
    logLocked("ob idle: idle session " + std::to_string(idleStarts_.load()) + " started (overlay on, no stream)");
    return true;
}

void OwenbPlayer::setIdleHint(const std::string& hint) {
    std::lock_guard<std::mutex> g(mu_);
    idleCfg_.hint = hint;
    if (idleWanted_ && !active_)
        ui_.setIdle(true, idleCfg_.name, hint);
}

bool OwenbPlayer::startIdle(const IdleConfig& cfg, std::string& err) {
    {
        std::lock_guard<std::mutex> g(mu_);
        idleCfg_ = cfg;
        if (cfg.log)
            log_ = cfg.log;
        aspect_.configure(aspectCfg_.enabled, aspectCfg_.store, log_);
        idleWanted_ = true;
        if (!active_) {
            if (!openRingLocked(err)) {
                idleWanted_ = false;
                return false;
            }
            startOsdLocked();
            ui_.setIdle(true, cfg.name, cfg.hint);
            if (!idleUp_)
                idleBeginLocked();
        }
    }
    if (!keeper_.joinable()) {
        keeperStop_ = false;
        keeper_ = std::thread([this] { keeperLoop(); });
    }
    return true;
}

void OwenbPlayer::stopIdle() {
    {
        std::lock_guard<std::mutex> g(mu_);
        idleWanted_ = false;
        keeperStop_ = true;
    }
    if (keeper_.joinable())
        keeper_.join();
    std::lock_guard<std::mutex> g(mu_);
    ui_.setIdle(false);
    if (!active_ && !session_)
        teardownLocked();   // a running play keeps the handle; its stop() closes it
}

bool OwenbPlayer::idleWanted() const {
    std::lock_guard<std::mutex> g(mu_);
    return idleWanted_;
}

bool OwenbPlayer::idleSessionOk() const {
    std::lock_guard<std::mutex> g(mu_);
    return idleWanted_ && !active_ && port_ && port_->status().session;
}

// T7: the idle session's keeper. While no play runs: the session lost (an OSD Reset, another program's
// session, the core reloaded under us) -> start it again, after a back-off once it fails; a restore the
// last play could not finish -> asked again here (no Letterbox on the idle session). Steps under mu_,
// sleeps outside it.
void OwenbPlayer::keeperLoop() {
    unsigned waitMs = 0;
    while (!keeperStop_.load()) {
        unsigned sleepMs = 0;
        std::function<void(unsigned)> sleeper;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (keeperStop_.load())
                break;
            sleepMs = idleCfg_.keepaliveMs ? idleCfg_.keepaliveMs : 250;
            sleeper = idleCfg_.sleepMs;
            if (!active_.load() && ring_ && port_) {
                if (idleCfg_.stepLocked)
                    idleCfg_.stepLocked();   // tests: the fake core's clock, only while no play steps it
                if (waitMs > 0) {
                    waitMs = waitMs > sleepMs ? waitMs - sleepMs : 0;
                } else {
                    const owenb_osd::OvlStatus st = port_->status();
                    if (st.valid && st.session && !idleRasterValid_) {
                        idleRaster_ = st.raster;
                        idleRasterValid_ = true;
                    }
                    if (!st.valid || !st.session) {
                        if (idleUp_)
                            logLocked("ob idle: the idle session was lost; starting it again");
                        idleUp_ = false;
                        idleRasterValid_ = false;   // F3: the restarted session records its raster again
#ifdef MUT_IDLE_NO_RESTART
                        if (idleStarts_.load() == 0 && !idleBeginLocked())   // mutant: a lost session stays lost
                            waitMs = kIdleRetryMs;
#else
                        if (!idleBeginLocked())
                            waitMs = kIdleRetryMs;
#endif
                    } else if (idleRasterValid_ && !(st.raster.w == idleRaster_.w && st.raster.h == idleRaster_.h &&
                                                      st.raster.interlaced == idleRaster_.interlaced &&
                                                      st.raster.videoOut == idleRaster_.videoOut)) {
                        // FIELD-1018 F3: a mode switch (OSD Native / Interlaced / Progressive, a video_mode change)
                        // with the session still up left the core's pattern on screen. Re-arm the idle session
                        // (twice, as the PLXH prime); the overlay renderer re-plans and repaints on the new raster.
                        logLocked("ob idle: video mode changed (" + std::to_string(idleRaster_.w) + "x" +
                                  std::to_string(idleRaster_.h) + " vout " + std::to_string(idleRaster_.videoOut) +
                                  " -> " + std::to_string(st.raster.w) + "x" + std::to_string(st.raster.h) + " vout " +
                                  std::to_string(st.raster.videoOut) + "); re-arming the idle card");
                        idleRaster_ = st.raster;
#ifndef MUT_F3_NO_REARM
                        ++idleRearms_;
                        if (!idleBeginLocked() || !idleBeginLocked())
                            waitMs = kIdleRetryMs;
                        // the renderer sees the new raster itself: hide, re-plan, a full repaint (black field + card)
#endif
                    } else if (aspect_.wantsStatus()) {
                        OwenbDdrWindow* w = window_.get();
                        aspect_.onStatus(*port_, st, [w](unsigned ms) { w->sleepUs(ms * 1000u); }, false);
                    }
                }
            }
        }
        if (sleeper)
            sleeper(sleepMs);
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }
}

bool OwenbPlayer::start(const OpenRequest& req, uint64_t sessionId, SessionConfig cfg, OwenbPlayCallbacks cb,
                        std::string& err) {
    stop();
    std::lock_guard<std::mutex> g(mu_);
    if (cb.log)
        log_ = cb.log;
    if (!openRingLocked(err))   // T7: on the idle session the handle is already open: the play reuses it
        return false;
    OwenbDdrWindow* w = window_.get();
    aspect_.configure(aspectCfg_.enabled, aspectCfg_.store, log_);
    aspect_.newSession(true);   // 0008: the one request waits for the stream's aspect (the first keyframe)
    aspectEnded_ = false;
    if (!cfg.sleepUs)
        cfg.sleepUs = [w](unsigned us) { w->sleepUs(us); };
    if (!cfg.log)
        cfg.log = cb.log;
    auto userIter = cfg.onIteration;
    auto lastProgress = std::make_shared<std::chrono::steady_clock::time_point>();
    // session_ is set before the thread starts and reset only after it is joined (stop).
    cfg.onIteration = [this, userIter, cb, w, lastProgress] {
        if (userIter)
            userIter();
        if (port_ && aspect_.wantsStatus())   // 0006: once, when the session is open
            aspect_.onStatus(*port_, port_->status(), [w](unsigned ms) { w->sleepUs(ms * 1000u); });
        if (cb.progress && session_ && session_->started()) {
            const auto now = std::chrono::steady_clock::now();
            if (*lastProgress == std::chrono::steady_clock::time_point{} ||
                now - *lastProgress >= std::chrono::milliseconds(1000)) {
                *lastProgress = now;
                cb.progress(session_->positionMs(), session_->paused());
            }
        }
        if (cb.position && session_)
            cb.position(session_->positionMs());
        if (session_ && osd_) {   // T6: the overlay follows the play (the thread rate-limits itself)
            const bool st = session_->started(), pa = session_->paused();
            const int64_t pos = session_->positionMs();
            ui_.update([&](owenb_osd::UiModel& m) {
                m.presented = st;
                m.paused = pa;
                m.positionMs = pos;
            });
        }
    };
    // 0008: each input (a seek reopens it) is read through the aspect sniffer; the first one decides
    SourceFactory sniffed = [this, base = sourceFactory_](const OpenRequest& r, std::string& e) -> std::unique_ptr<IMediaSource> {
        std::unique_ptr<IMediaSource> s = base(r, e);
        if (!s)
            return s;
        return std::make_unique<AspectSniffSource>(std::move(s), aspect_, log_);
    };
    session_ = std::make_unique<OwenbPlaySession>(*ring_, sniffed, cfg);
    session_->onFirstPicture = cb.started;
    session_->setVolumePercent(volume_);
    ui_.update([](owenb_osd::UiModel& m) { m.idle = false; });
    startOsdLocked();
    active_ = true;
    idleUp_ = false;   // the play's session replaces the idle one
    OwenbPlaySession* s = session_.get();
    thr_ = std::thread([this, s, req, sessionId, cb, w] {
        const EndReason why = s->run(req, sessionId);
        endAspect(w);   // 0006: every end (stop, end of stream, error) restores the user's Analog Aspect
        {
            std::lock_guard<std::mutex> lk(mu_);   // stop() never holds mu_ across the join
            if (idleWanted_) {   // T7: back to the idle card; the play's session goes on as the idle one
                idleUp_ = true;
                ui_.setIdle(true, idleCfg_.name, idleCfg_.hint);
            }
        }
        if (cb.ended)
            cb.ended(why, s->error(), s->positionMs());
        active_ = false;
    });
    return true;
}

void OwenbPlayer::endAspect(OwenbDdrWindow* w) {
    if (aspectEnded_.exchange(true) || !port_)
        return;
    lastAspectEnd_ = aspect_.endSession(*port_, [w](unsigned ms) {
        if (w)
            w->sleepUs(ms * 1000u);
    });
}

void OwenbPlayer::setAspect(AspectConfig cfg) {
    std::lock_guard<std::mutex> g(mu_);
    aspectCfg_ = std::move(cfg);
}

void OwenbPlayer::setOsd(OsdConfig cfg) {
    std::lock_guard<std::mutex> g(mu_);
    osdCfg_ = std::move(cfg);
}

owenb_osd::RendererStats OwenbPlayer::osdStats() const {
    std::lock_guard<std::mutex> g(mu_);
    return osd_ ? osd_->stats() : osdLast_;
}

int OwenbPlayer::readInput(struct ::h264ring_input& in) const {
    std::lock_guard<std::mutex> g(mu_);
    return ring_ ? ring_->input(in) : -ENODEV;
}

bool OwenbPlayer::osdRunning() const {
    std::lock_guard<std::mutex> g(mu_);
    return osd_ != nullptr;
}

bool OwenbPlayer::started() const {
    std::lock_guard<std::mutex> g(mu_);
    return session_ && session_->started();
}

bool OwenbPlayer::paused() const {
    std::lock_guard<std::mutex> g(mu_);
    return session_ && session_->paused();
}

int64_t OwenbPlayer::positionMs() const {
    std::lock_guard<std::mutex> g(mu_);
    return session_ ? session_->positionMs() : 0;
}

void OwenbPlayer::pause() {
    std::lock_guard<std::mutex> g(mu_);
    if (session_ && active_)
        session_->pause();
}

void OwenbPlayer::resume() {
    std::lock_guard<std::mutex> g(mu_);
    if (session_ && active_)
        session_->resume();
}

void OwenbPlayer::seek(const OpenRequest& req) {
    std::lock_guard<std::mutex> g(mu_);
    if (session_ && active_)
        session_->seek(req);
}

void OwenbPlayer::setVolumePercent(int volume) {
    std::lock_guard<std::mutex> g(mu_);
    volume_ = volume;
    if (session_ && active_)
        session_->setVolumePercent(volume);
}

void OwenbPlayer::stop() {
    std::thread t;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (session_)
            session_->stop();
        t = std::move(thr_);
    }
    if (t.joinable())
        t.join();
    std::lock_guard<std::mutex> g(mu_);
    endAspect(window_.get());   // 0006: a play thread that never got to its end (teardown)
    active_ = false;
#ifdef MUT_IDLE_TEARDOWN_ON_STOP
    if (false) {   // mutant: every stop closes the handle (the idle card gone after a play)
#else
    if (idleWanted_ && ring_) {   // T7: back to the idle session: the handle, fb0 and the overlay stay
#endif
        session_.reset();
        ui_.setIdle(true, idleCfg_.name, idleCfg_.hint);
        return;
    }
    teardownLocked();
}

void OwenbPlayer::queueDepths(unsigned long long& out, unsigned long long& ready,
                              unsigned long long& probe) const {
    std::lock_guard<std::mutex> g(mu_);
    if (!session_) {
        out = ready = probe = 0;
        return;
    }
    out = session_->outDepth();
    ready = session_->readyDepth();
    probe = session_->probeBound();
}

} // namespace misterplex::owenb
