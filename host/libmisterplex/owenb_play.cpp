// OWENB-P1B T5: the daemon's OB play worker (owenb_play.hpp). T7: the long-lived idle session.
#include "owenb_play.hpp"

#include "ob_glass.hpp"
#include "owenb_aspect.hpp"
#include "picture_14_9.hpp"
#include "remember_picture.hpp"

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
        if (r != SourceRead::Packet || p.kind != MediaPacket::Kind::Video)
            return r;
        if (!done_) {
            int64_t cn = 0, cd = 0;
            const bool hc = in_->containerAspect(cn, cd);
            const OwenbStreamAspect a = owenbStreamAspect(p.data.data(), p.data.size(), hc, cn, cd);
            if (!a.spsFound && !p.keyframe && ++video_ < kGiveUp) {
                // keep sniffing; a later access unit may carry the SPS
            } else {
                done_ = true;
                if (!g_.stream().known) {   // an earlier input of this play (before a seek) decided
                    owenb_osd::StreamAspect sa;
                    sa.known = true;
                    sa.force43 = a.force43;
                    sa.square43 = a.square43;
                    g_.setStream(sa);
                    if (log_ && a.spsFound && !a.spsAspect)
                        log_("aspect: the stream's SPS has no aspect (the core reads 16:9); display " +
                             std::to_string(a.darNum) + ":" + std::to_string(a.darDen) +
                             (hc ? " (container)" : " (picture)") +
                             (a.force43 ? (a.square43 ? " -> a 4:3 candidate (square samples; the guard decides by Video Output)"
                                                     : " -> a 4:3 candidate (the guard decides)")
                                        : ""));
                }
            }
        }
        // Sniff used the original SPS. 14:9 then narrows the display window the core already letterboxes.
        if (maybeApplyPicture149(p.data) && log_ && !logged149_) {
            logged149_ = true;
            log_("picture: 14:9 side crop; Letterbox on the 4:3 raster");
        }
        return r;
    }
    void cancel() override { in_->cancel(); }
    bool containerAspect(int64_t& num, int64_t& den) const override { return in_->containerAspect(num, den); }
    size_t readyDepth() const override { return in_->readyDepth(); }
    unsigned probePackets() const override { return in_->probePackets(); }
    int64_t startPts90k() const override { return in_->startPts90k(); }
    StreamSwitch selectAudioStream(int audioIndex, std::string& err) override {
        return in_->selectAudioStream(audioIndex, err);
    }
    StreamSwitch selectSubtitleStream(int subtitleIndex, std::string& err) override {
        return in_->selectSubtitleStream(subtitleIndex, err);
    }
    std::string activeSubtitle(int64_t positionMs) override { return in_->activeSubtitle(positionMs); }
    int activeAudioIndex() const override { return in_->activeAudioIndex(); }
    bool audioDecoderOpen() const override { return in_->audioDecoderOpen(); }

private:
    std::unique_ptr<IMediaSource> in_;
    owenb_osd::AspectGuard& g_;
    std::function<void(const std::string&)> log_;
    bool done_ = false;
    bool logged149_ = false;
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
void OwenbPlayer::setBeforeRingOpen(std::function<void()> fn) {
    std::lock_guard<std::mutex> g(mu_);
    beforeRingOpen_ = std::move(fn);
}

void OwenbPlayer::setOnOsdStarted(std::function<void()> fn) {
    std::function<void()> run;
    {
        std::lock_guard<std::mutex> g(mu_);
        onOsdStarted_ = std::move(fn);
        if (osd_ && onOsdStarted_)
            run = onOsdStarted_;
    }
    if (run)
        run();
}

bool OwenbPlayer::openRingLocked(std::string& err) {
    if (ring_)
        return true;
    if (beforeRingOpen_)
        beforeRingOpen_();
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
    // Do not invoke onOsdStarted_ here: callers hold mu_, and the MediaPlayer
    // callback takes obMu_. Opposite order is owenbDecodedPicture / owenbIdleWanted
    // / owenbRingProducer (obMu_ then Owenb mu_).
    osdStartedFire_ = onOsdStarted_;
}

void OwenbPlayer::fireOsdStarted() {
    std::function<void()> run;
    {
        std::lock_guard<std::mutex> g(mu_);
        run.swap(osdStartedFire_);
    }
    if (run)
        run();
}

// Everything off: the overlay hidden (while the handle is open), fb0 unmapped, the handle closed
// (h264ring_close: the lock and the rumble go, h264ring.h:94), the window unmapped.
void OwenbPlayer::teardownLocked() {
    osdStartedFire_ = {};
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
    // GBE N-7: drive Interlaced letterbox guard from Spelunkr state on idle too
    {
        const int ar = spelunkrAspect_.load();
        aspect_.setSpelunkrMode(ar == 1 || ar == 2);
    }
    applySpelunkrAspectLocked();
    logLocked("ob idle: idle session " + std::to_string(idleStarts_.load()) + " started (overlay on, no stream)");
    return true;
}

bool OwenbPlayer::ensureRingOpen(std::string& err) {
    std::lock_guard<std::mutex> g(mu_);
    return openRingLocked(err);
}

h264stream::IAvRecordProducer* OwenbPlayer::ringProducer() {
    std::lock_guard<std::mutex> g(mu_);
    return ring_.get();
}

bool OwenbPlayer::mediaPlayActive() const {
    std::lock_guard<std::mutex> g(mu_);
    return session_ && active_.load();
}

bool OwenbPlayer::claimExternalPlay(std::string& err) {
    bool ok = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (session_ && active_.load()) {
            err = "an Owenb play session is running";
            return false;
        }
        if (!openRingLocked(err))
            return false;
        startOsdLocked();
        active_ = true;
        idleUp_ = false;   // keeper skips idle re-arm while active_
        ui_.update([](owenb_osd::UiModel& m) { m.idle = false; });
        ok = true;
    }
    fireOsdStarted();
    return ok;
}

void OwenbPlayer::releaseExternalPlay() {
    std::lock_guard<std::mutex> g(mu_);
    if (session_)
        return;   // a real play took the handle; leave it
    active_ = false;
    if (idleWanted_ && ring_) {
        // PLAY-PENDING owns the screen. Re-arming the idle session must not put the card back.
        if (!ui_.snapshot().pending)
            ui_.setIdle(true, idleCfg_.name, idleCfg_.hint);
        idleBeginLocked();
    }
}

void OwenbPlayer::setIdleHint(const std::string& hint) {
    std::lock_guard<std::mutex> g(mu_);
    idleCfg_.hint = hint;
    if (idleWanted_ && !active_ && !ui_.snapshot().pending && !ui_.snapshot().surface)
        ui_.setIdle(true, idleCfg_.name, hint);
}

void OwenbPlayer::setIdleName(const std::string& name) {
    std::lock_guard<std::mutex> g(mu_);
    idleCfg_.name = name;
    if (idleWanted_ && !active_ && !ui_.snapshot().pending)
        ui_.setIdle(true, name, idleCfg_.hint);
}

bool OwenbPlayer::startIdle(const IdleConfig& cfg, std::string& err) {
    bool already = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        idleCfg_ = cfg;
        if (cfg.log)
            log_ = cfg.log;
        aspect_.configure(aspectCfg_.enabled, aspectCfg_.store, log_);
        // GH#27: a second startIdle (gate flicker, duplicate tick) must not
        // begin() again or setIdle(Ready) over plexui.
        if (idleWanted_ && osd_) {
            if (!active_ && !idleUp_)
                idleBeginLocked();
            already = true;
        } else {
            idleWanted_ = true;
            if (!active_) {
                if (!openRingLocked(err)) {
                    idleWanted_ = false;
                    return false;
                }
                startOsdLocked();
                // owenbIdleTick/startIdle must not re-show the card while PLAY-PENDING
                // or while plexui already owns the overlay (GH#27 Ready-over-browse).
                if (!ui_.snapshot().pending && !ui_.snapshot().surface)
                    ui_.setIdle(true, cfg.name, cfg.hint);
                if (!idleUp_)
                    idleBeginLocked();
            }
        }
    }
    if (!already)
        fireOsdStarted();
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
                    if (st.valid)
                        picture149NoteVout(static_cast<int>(st.raster.videoOut));
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
                    } else if (idleRearmReq_.exchange(false) && idleUp_ && idleWanted_) {
                        // An event the raster dimensions do not show (a mode/timing change with the same size, a
                        // core load adopted, a recovery): the same re-arm as a raster change, on the next tick.
                        logLocked("ob idle: re-arm requested by a mode/core event; re-arming the idle card");
                        idleRaster_ = st.raster;
#ifndef MUT_IDLE_REARM_REQ_IGNORED
                        ++idleRearms_;
                        if (!idleBeginLocked() || !idleBeginLocked())
                            waitMs = kIdleRetryMs;
#endif
                    } else if (idleRasterValid_ && idleRasterCredible(st.raster.w, st.raster.h) &&
                               !(st.raster.w == idleRaster_.w && st.raster.h == idleRaster_.h &&
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
    // The accept (or playOwenbRoute) already stored this session's gen. Capture it
    // before stop() joins the previous thread, whose epilogue must not adopt it.
    const uint64_t myPending = ui_.snapshot().pendingGen;
    stop();
    {
        std::lock_guard<std::mutex> g(mu_);
    if (cb.log)
        log_ = cb.log;
    if (!openRingLocked(err))   // T7: on the idle session the handle is already open: the play reuses it
        return false;
    OwenbDdrWindow* w = window_.get();
    aspect_.configure(aspectCfg_.enabled, aspectCfg_.store, log_);
    aspect_.newSession(true);   // 0008: the one request waits for the stream's aspect (the first keyframe)
    const int voutKeep = picture149VoutSlot().load();
    picture149BeginPlay();
    pictureDirty_.store(false);
    const int armed = pictureArm_.exchange(-1);
    // Explicit/remembered picture modes win over Spelunkr Auto aspect defaults.
    // GBE N-7: shared armAspectAfterNewSessionLocked covers both branches.
    armAspectAfterNewSessionLocked(armed);
    if (port_) {
        const owenb_osd::OvlStatus st = port_->status();
        if (st.valid)
            picture149NoteVout(static_cast<int>(st.raster.videoOut));
        else if (voutKeep > 0)
            picture149NoteVout(voutKeep);
    }
    aspectEnded_ = false;
    if (!cfg.sleepUs)
        cfg.sleepUs = [w](unsigned us) { w->sleepUs(us); };
    if (!cfg.log)
        cfg.log = cb.log;
    auto userIter = cfg.onIteration;
    auto lastProgress = std::make_shared<std::chrono::steady_clock::time_point>();
    auto playStart = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    // session_ is set before the thread starts and reset only after it is joined (stop).
    cfg.onIteration = [this, userIter, cb, w, lastProgress, playStart] {
        if (userIter)
            userIter();
        if (port_) {
            owenb_osd::OvlStatus st = port_->status();
            if (st.valid)
                picture149NoteVout(static_cast<int>(st.raster.videoOut));
            if (pictureDirty_.exchange(false)) {
                // User Program-menu PICTURE pick: apply geometry without Spelunkr
                // Auto-only defaults stomping the choice (Spelunkr-only regression).
                const int mode = pictureLive_.load();
                if (mode < 0 && spelunkrAspect_.load() >= 0) {
                    picture149Choice().store(-1);
                    aspect_.endSession(*port_, [w](unsigned ms) { w->sleepUs(ms * 1000u); });
                    armAspectAfterNewSessionLocked(mode);
                    st = port_->status();
                } else {
                    applyArmedPicture(mode);
                }
                if (st.valid && !aspect_.wantsStatus())
                    aspect_.reapplyPicture(*port_, st);
            }
            if (aspect_.wantsStatus())   // 0006: once, when the session is open
                aspect_.onStatus(*port_, st, [w](unsigned ms) { w->sleepUs(ms * 1000u); });
        }
        int64_t pos = 0;
        uint32_t stamp = 0;
        if (session_)
            session_->readPosition(pos, stamp);
        // Publish before the 1 Hz progress tick. Both carry one read of the publication,
        // so a seek that landed between them cannot pair this position with a newer stamp.
        if (cb.position && session_)
            cb.position(pos, stamp);
        if (cb.media && session_) {
            int64_t media = 0;
            uint32_t mediaStamp = 0;
            session_->readMedia(media, mediaStamp);
            cb.media(media, mediaStamp, session_->paused());
        }
        if (cb.progress && session_ && session_->started()) {
            const auto now = std::chrono::steady_clock::now();
            if (*lastProgress == std::chrono::steady_clock::time_point{} ||
                now - *lastProgress >= std::chrono::milliseconds(1000)) {
                *lastProgress = now;
                cb.progress(pos, session_->paused());
                // AirPlay and YouTube on PlexOB never reach the FPGA frames= log.
                // Glass reads pfps from this line (cb.log is prefixed "media: ").
                if (cb.log) {
                    const double wall = std::chrono::duration<double>(now - *playStart).count();
                    cb.log(playedFramesLine(session_->stats().videoAus, wall));
                }
            }
        }
        if (session_ && osd_) {   // T6: the overlay follows the play (the thread rate-limits itself)
            const bool st = session_->started(), pa = session_->paused();
            ui_.update([&](owenb_osd::UiModel& m) {
                m.presented = st;
                m.paused = pa;
                m.positionMs = pos;
            });
        }
    };
    // 0008: each input (a seek reopens it) is read through the aspect sniffer; the first one decides
    SourceFactory sniffed = [this, base = sourceFactory_](const OpenRequest& r, std::string& e) -> std::unique_ptr<IMediaSource> {
        std::unique_ptr<IMediaSource> s = r.prefetched ? r.prefetched->take() : nullptr;
        if (!s)
            s = base(r, e);
        if (!s)
            return s;
        if (spelunkrAspect_.load() >= 0) {
            const auto format = s->videoFormat();
            logLocked("spelunkr: source " + std::to_string(format.width) + "x" + std::to_string(format.height) +
                      (format.interlaced ? " interlaced" : " progressive") + " fps=" + std::to_string(format.fpsNum) +
                      "/" + std::to_string(format.fpsDen) +
                      " Video Output/Refresh: user OSD values retained; core follows source only in Auto");
        }
        return std::make_unique<AspectSniffSource>(std::move(s), aspect_, log_);
    };
    session_ = std::make_unique<OwenbPlaySession>(*ring_, sniffed, cfg);
    // RAOP keeps the idle card until this fires (the spectrum's first picture).
    // Other plays drop it now, and this callback is only the caller's started().
    const bool keepIdle = cfg.keepIdleUntilPicture;
    auto userStarted = cb.started;
    session_->onFirstPicture = [this, userStarted, keepIdle, myPending] {
        // First HDMI picture of this gen: chevron down. A newer accept's gen stays.
        // RAOP still drops the idle card here (keepIdleUntilPicture).
        // GH#410: start the 2 s overlay-clock hide. Clearing the bar here left it
        // up for the whole play when this callback missed.
        const int64_t now = osdCfg_.clockMs
                                ? osdCfg_.clockMs()
                                : int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
        bool ours = true;
        ui_.update([keepIdle, myPending, now, &ours](owenb_osd::UiModel& m) {
            if (m.pendingGen != 0 && myPending != m.pendingGen) {
                ours = false;
                return;
            }
            m.pending = false;
            m.pendingGen = 0;
            m.pendingLabel.clear();
            if (m.spelunkrTuning)
                m.armSpelunkrBannerHide(now);
            m.presented = true;
            m.buffering = false;
            if (keepIdle)
                m.idle = false;
        });
        if (ours && userStarted)
            userStarted();
    };
    session_->setVolumePercent(volume_);
    if (dropIdleCardAtPlayStart(cfg.keepIdleUntilPicture))
        ui_.update([](owenb_osd::UiModel& m) { m.idle = false; });
    startOsdLocked();
    active_ = true;
    idleUp_ = false;   // the play's session replaces the idle one
    OwenbPlaySession* s = session_.get();
    thr_ = std::thread([this, s, req, sessionId, cb, w, myPending] {
        const EndReason why = s->run(req, sessionId);
        endAspect(w);   // 0006: every end (stop, end of stream, error) restores the user's Analog Aspect
        {
            std::lock_guard<std::mutex> lk(mu_);   // stop() never holds mu_ across the join
            if (idleWanted_) {   // T7: back to the idle card; the play's session goes on as the idle one
                idleUp_ = true;
                const owenb_osd::UiModel snap = ui_.snapshot();
                // A newer play/cast already armed PLAY-PENDING. This session's stop
                // (the handoff inside start()) must not cover that chevron with the card.
                const bool newerCue = snap.pending && snap.pendingGen != myPending;
                // Natural TV EOF keeps the held frame until the same-channel
                // schedule worker has prefetched its successor (or reports failure).
                if (!newerCue) {
                    if (snap.spelunkr && why == EndReason::Eof)
                        ui_.armPlayPending(sessionId, {}, snap.spelunkrDial, snap.spelunkrName);
                    else if (snap.spelunkr && spelunkrFailEndHoldsFrame(why))
                        // P0 11-10: a mid-play failure skips the item; tuning is set BEFORE any idle
                        // paint, so the held frame + banner stay until the next item's first picture.
                        ui_.armPlayPending(sessionId, {},
                                           snap.spelunkrDial.empty() ? std::string("Spelunkr") : snap.spelunkrDial,
                                           snap.spelunkrName);
                    else
                        ui_.setIdle(true, idleCfg_.name, idleCfg_.hint);
                }
            }
        }
        if (cb.ended)
            cb.ended(why, s->error(), s->positionMs());
        active_ = false;
    });
    }
    fireOsdStarted();
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

void OwenbPlayer::armPicture(int mode) { pictureArm_.store(mode); }

int OwenbPlayer::pictureModeForPlay(int rememberedMode) const {
    const int sticky = spelunkrPicture_.load();
    return spelunkrAspect_.load() >= 0 && sticky >= -1 ? sticky : rememberedMode;
}

void OwenbPlayer::armSpelunkrAspect(int ar) {
    spelunkrAspect_.store(ar == 1 || ar == 2 ? ar : -1);
}

void OwenbPlayer::unitTestArmPlayAspect(int spelunkrAr, int armedPictureMode) {
    std::lock_guard<std::mutex> g(mu_);
    spelunkrAspect_.store(spelunkrAr == 1 || spelunkrAr == 2 ? spelunkrAr : -1);
    aspect_.newSession(true);
    armAspectAfterNewSessionLocked(armedPictureMode);
}


void OwenbPlayer::armAspectAfterNewSessionLocked(int armedPictureMode) {
    armedPictureMode = pictureModeForPlay(armedPictureMode);
    const int spelunkrAr = spelunkrAspect_.load();
    // GBE N-7: Spelunkr mode after newSession, before PICTURE branch (covers armed + Auto).
    if (spelunkrAr == 1 || spelunkrAr == 2)
        aspect_.setSpelunkrMode(true);
    if (armedPictureMode >= 0) {
        pictureLive_.store(armedPictureMode);
        applyArmedPicture(armedPictureMode);
    } else {
        pictureLive_.store(-1);
        applySpelunkrAspectLocked();
    }
}

void OwenbPlayer::applySpelunkrAspectLocked() {
    const int ar = spelunkrAspect_.load();
    if (ar != 1 && ar != 2)
        return;
    aspect_.setSpelunkrMode(true);
    const int sticky = spelunkrPicture_.load();
    if (sticky >= 0) {
        applyArmedPicture(sticky);
        return;
    }
    // Live or armed picture modes are explicit user/remembered choices.
    if (pictureLive_.load() >= 0 || pictureArm_.load() >= 0)
        return;
    aspect_.setPicture(true, owenb_osd::AspectGuard::kLetterbox, true, static_cast<unsigned>(ar), false, 0, true, true);
}

void OwenbPlayer::requestPicture(int mode) {
    if (spelunkrAspect_.load() >= 0)
        spelunkrPicture_.store(mode);
    pictureLive_.store(mode);
    pictureDirty_.store(true);
}

void OwenbPlayer::applyArmedPicture(int mode) {
    const std::optional<PictureSpec> spec = pictureSpecForArm(mode);
    if (!spec) {
        aspect_.clearPicture();
        return;
    }
    picture149Choice().store(spec->crop149 ? 1 : 0);
    aspect_.setPicture(true, spec->analogAspect, spec->setAspectRatio, spec->aspectRatio);
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

int OwenbPlayer::syncService(bool spelunkr, const unsigned* plexSettings) {
    std::lock_guard<std::mutex> g(mu_);
    if (!ring_)
        return -ENODEV;
    if (plexSettings) {
        const int rc = ring_->requestSettings(0x7fu, plexSettings);
        if (rc < 0)
            return rc;
        // Do not let the Service status_set mask an in-flight video restore.
        // Require the real settings sequence/result before sending word 38.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        bool applied = false;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto status = ring_->overlayStatus();
            if (status.setSeq == static_cast<unsigned>(rc)) {
                if (status.setErr != 0)
                    return -EINVAL;
                applied = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!applied)
            return -ETIMEDOUT;
    }
    return ring_->syncService(spelunkr);
}
bool OwenbPlayer::osdRunning() const {
    std::lock_guard<std::mutex> g(mu_);
    return osd_ != nullptr;
}

bool OwenbPlayer::started() const {
    std::lock_guard<std::mutex> g(mu_);
    return session_ && session_->started();
}

bool OwenbPlayer::decodedPicture() const {
    std::lock_guard<std::mutex> g(mu_);
    return session_ && session_->decodedPicture();
}

bool OwenbPlayer::paused() const {
    std::lock_guard<std::mutex> g(mu_);
    return session_ && session_->paused();
}

int64_t OwenbPlayer::positionMs() const {
    std::lock_guard<std::mutex> g(mu_);
    return session_ ? session_->positionMs() : 0;
}

void OwenbPlayer::readPosition(int64_t& ms, uint32_t& stamp) const {
    std::lock_guard<std::mutex> g(mu_);
    if (session_)
        session_->readPosition(ms, stamp);
    else {
        ms = 0;
        stamp = 0;
    }
}

int64_t OwenbPlayer::mediaMs() const {
    std::lock_guard<std::mutex> g(mu_);
    return session_ ? session_->mediaMs() : 0;
}

void OwenbPlayer::readMedia(int64_t& ms, uint32_t& stamp) const {
    std::lock_guard<std::mutex> g(mu_);
    if (session_)
        session_->readMedia(ms, stamp);
    else {
        ms = 0;
        stamp = 0;
    }
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

void OwenbPlayer::selectStreams(int audioIndex, bool setAudio, int subtitleIndex, bool setSubtitle,
                                int textContainerIndex, misterplex::PlexTextIdentity text) {
    std::lock_guard<std::mutex> g(mu_);
    if (session_ && active_)
        session_->selectStreams(audioIndex, setAudio, subtitleIndex, setSubtitle, textContainerIndex,
                                std::move(text));
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
        // PLAY-PENDING (a newer accept, or this play still waiting on HDMI) keeps the chevron.
        if (!ui_.snapshot().pending)
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
