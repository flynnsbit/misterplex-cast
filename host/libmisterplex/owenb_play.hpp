#pragma once
// OWENB-P1B T5: the daemon's OB play worker. It owns one OB play: the DDR window (the daemon's is
// DevMemDdrWindow on MPX_DEVMEM_PATH: the ctrl page and the ring only, under upstream's flock), the
// OwenbRing on it and an OwenbPlaySession on its own thread. MediaPlayer forwards the cast timeline's
// controls here (pause / resume / seek / stop / volume) and reports what the callbacks tell it.
// Nothing here touches the idle framebuffer (0x31600000) or paints DDR: the window maps neither.
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "owenb_aspect_guard.hpp"
#include "owenb_ddr_window.hpp"
#include "owenb_osd_renderer.hpp"
#include "owenb_ring.hpp"
#include "owenb_session.hpp"
#include "owenb_prefetch.hpp"

#include "idle_raster_credible.hpp"

namespace misterplex::owenb {

struct OwenbPlayCallbacks {
    std::function<void()> started;                                   // the first picture (play thread)
    // Every loop pass (play thread). stamp is the seek epoch and phase (posStamp): a
    // sample from before the seek loses to the landing already committed.
    std::function<void(int64_t positionMs, uint32_t stamp)> position;
    // Picture time (readMedia), same thread as position. The bar still uses position.
    std::function<void(int64_t mediaMs, uint32_t stamp, bool paused)> media;
    // After session_->started(): at most once per second (PMS /:/timeline + companion poll time).
    std::function<void(int64_t positionMs, bool paused)> progress;
    // The play ended on its own or by stop(); called on the play thread, before it exits. It must not
    // call OwenbPlayer::stop() (that joins this thread).
    std::function<void(EndReason why, const std::string& error, int64_t positionMs)> ended;
    std::function<void(const std::string&)> log;
};

using WindowFactory = std::function<std::unique_ptr<OwenbDdrWindow>(std::string& err)>;

// OWENB-P1B T6: the ARM overlay for the OB play (owenb_osd_renderer.hpp). mapFb maps fb0
// (h264ring_fb_map in the daemon; a heap page at H264RING_FB_BYTE in the tests); no mapFb or
// enabled = false (MPX_OB_OSD=0): no overlay thread. allowDouble = false forces the single-page
// path (MPX_OB_OSD_SINGLE=1).
struct OsdConfig {
    bool enabled = true;
    bool allowDouble = true;
    std::function<bool(owenb_osd::FbMap& fb, std::string& err)> mapFb;
    std::function<void()> unmapFb;
    std::function<int64_t()> clockMs;   // default: steady_clock
    std::function<void(uint64_t)> onSurfaceShown;   // OB port: a PlexUI frame is on screen (renderer thread)
};

// 0006: the Analog Aspect default (owenb_aspect_guard.hpp), independent of the overlay: enabled
// unless MPX_OB_LETTERBOX=0; store = the state file (MPX_OB_ASPECT_STATE) in the daemon, memory in tests.
struct AspectConfig {
    bool enabled = true;
    std::shared_ptr<owenb_osd::IAspectStore> store;
};

// The OwenbRing as the renderer's port: each call takes the ring's mutex for microseconds.
class RingOverlayPort final : public owenb_osd::IOverlayPort {
public:
    explicit RingOverlayPort(OwenbRing& r) : ring_(r) {}
    int show(const struct ::h264ring_overlay& o) override { return ring_.overlayShow(o); }
    int hide() override { return ring_.overlayHide(); }
    owenb_osd::OvlStatus status() override {
        const OverlayCoreStatus c = ring_.overlayStatus();
        owenb_osd::OvlStatus s;
        s.valid = c.valid;
        s.gen = c.gen;
        s.state = c.state;
        s.err = c.err;
        s.at = c.at;
        // GH#412: layout follows HDMI raster + h264ring_video_out, not OSD_VIDEO_OUT.
        // even-lines come from raster.interlaced (leftover Progressive word must not drop them).
        s.raster = owenb_osd::fromCore(c.raster, c.videoOut);
        s.osdAnalogAspect = c.osdAnalogAspect;
        s.osdAspect = c.osdAspect;
        s.osdVideoOut = c.osdVideoOut;
        s.setSeq = c.setSeq;
        s.setErr = c.setErr;
        s.session = c.session;
        return s;
    }
    int requestSettings(unsigned mask, const unsigned v[H264RING_OPT_COUNT]) override {
        return ring_.requestSettings(mask, v);
    }
    int clearSettings() override { return ring_.clearSettings(); }

private:
    OwenbRing& ring_;
};

// T7: the long-lived idle session (design §c.4 rule 2): at gate open the daemon starts a session with no
// stream on the PlexOB core, so the overlay (the idle card, the error screens) shows without a play
// (h264ring.h:286). A play reuses the handle; its end goes back to the idle card.
struct IdleConfig {
    std::string name;                // the device's name on the card
    std::string hint;                // "" -> Ready to cast; SPELUNKR uses Open the Guide
    unsigned keepaliveMs = 250;      // how often the keeper checks the session
    std::function<void(const std::string&)> log;
    std::function<void(unsigned)> sleepMs;   // the keeper's sleep (default: the thread sleeps)
    std::function<void()> stepLocked;        // tests: the fake core's clock, while no play runs
};

class OwenbPlayer {
public:
    static constexpr uint64_t kIdleSessionId = 0x1D1E5E55ull;
    static constexpr unsigned kIdleRetryMs = 2000;   // a failed idle start: try again after this
    OwenbPlayer(WindowFactory window, SourceFactory source);
    ~OwenbPlayer();
    OwenbPlayer(const OwenbPlayer&) = delete;
    OwenbPlayer& operator=(const OwenbPlayer&) = delete;

    // Stops a running play first. False (err set) when the window does not open or the core is not
    // alive: nothing was started and no callback runs.
    bool start(const OpenRequest& req, uint64_t sessionId, SessionConfig cfg, OwenbPlayCallbacks cb,
               std::string& err);
    std::shared_ptr<PrefetchedInput> prefetch(const OpenRequest& req, const std::function<bool()>& cancelled,
                                            std::string& err, PrefetchLimits limits = {},
                                            bool* timedOut = nullptr) {
        return prefetchInput(sourceFactory_, req, cancelled, err, limits, timedOut);
    }
    bool active() const { return active_.load(); }
    bool started() const;
    bool decodedPicture() const;   // this play's session showed a real decoded picture
    bool paused() const;
    int64_t positionMs() const;
    void readPosition(int64_t& ms, uint32_t& stamp) const;
    // True media time and its seek stamp. The bar keeps using positionMs().
    int64_t mediaMs() const;
    void readMedia(int64_t& ms, uint32_t& stamp) const;

    void pause();
    void resume();
    void seek(const OpenRequest& req);
    void setVolumePercent(int volume);
    // Container indexes. The play thread switches in place, or reopens at the current position.
    void selectStreams(int audioIndex, bool setAudio, int subtitleIndex, bool setSubtitle,
                       int textContainerIndex = -2, misterplex::PlexTextIdentity text = {});
    void stop();   // stops the play and joins its thread (never from a callback); on the idle session: back to it

    // T7: the idle session. startIdle opens the handle (if no play has), maps fb0, shows the idle card and
    // starts the session and its keeper; false (err) when the window does not open or the core is not alive
    // (the caller retries). stopIdle: the keeper stops; with no play running everything closes (a hop off OB).
    bool startIdle(const IdleConfig& cfg, std::string& err);
    void stopIdle();
    bool idleWanted() const;
    bool idleSessionOk() const;   // the idle session is up (no play, the core runs this handle's session)
    unsigned idleStarts() const { return idleStarts_.load(); }
    unsigned idleRearms() const { return idleRearms_.load(); }   // FIELD-1018 F3
    // Any mode/raster/core event (display raster apply, core load adopted, recovery): re-arm the idle card on the
    // keeper's next tick even when the raster dimensions did not change. No-op without an idle session.
    void requestIdleRearm() { idleRearmReq_.store(true); }

    // AirPlay screen-mirror: reuse this player's OwenbRing (the same handle idle and Plex play use).
    // Does not start OwenbPlaySession. Plex start()/stop() stay on their existing path.
    bool ensureRingOpen(std::string& err);
    h264stream::IAvRecordProducer* ringProducer();
    bool mediaPlayActive() const;   // a real OwenbPlaySession, not an external claim
    bool claimExternalPlay(std::string& err);
    void releaseExternalPlay();

    // T6: the overlay. setOsd before start (the next play uses it); ui() lives as long as the
    // player, across plays, so MediaPlayer can post a toast for the next play (R6p's restart).
    void setOsd(OsdConfig cfg);
    owenb_osd::UiBus& ui() { return ui_; }
    owenb_osd::RendererStats osdStats() const;
    bool osdRunning() const;
    // 0010: a copy of the core's input region (OwenbRing::input) while the handle is open (the idle
    // session or a play); -ENODEV when it is not. The input thread polls this (MediaPlayer).
    int readInput(struct ::h264ring_input& in) const;
    // 0006: the Letterbox default and the restore of the user's value (lives across plays).
    void setAspect(AspectConfig cfg);
    // REMEMBER_PICTURE. -1 is the tip path. armPicture is read once at start.
    // requestPicture applies on the play thread (the next iteration).
    void armPicture(int mode);
    void requestPicture(int mode);
    // A live Spelunkr menu pick (including -1 Auto) wins over per-show memory.
    int pictureModeForPlay(int rememberedMode) const;
    // SPELUNKR=1: OSD Aspect Ratio 4:3 (1) or 16:9 (2), Analog Aspect Letterbox.
    // -1 clears. Applied after newSession so it is not wiped.
    void armSpelunkrAspect(int ar);
    // Unit-test hook: same aspect arm as start() after newSession, without opening the ring.
    // spelunkrAr: -1 Plex, 1/2 Spelunkr. armedPictureMode: -1 none, else REMEMBER_PICTURE mode.
    void unitTestArmPlayAspect(int spelunkrAr, int armedPictureMode = -1);
    owenb_osd::AspectGuard& unitTestAspect() { return aspect_; }
    int syncService(bool spelunkr, const unsigned* plexSettings = nullptr);
    // Boot hand-over (PR #30 follow-up 10-23): called, with the player's lock held, immediately BEFORE the DDR window
    // factory opens the ring -- the one moment another ring holder (the "daemon not running" painter) has to let go.
    // The daemon sets it so the painter stays on screen until the daemon can paint its own card, not from main().
    void setBeforeRingOpen(std::function<void()> fn);
    void setOnOsdStarted(std::function<void()> fn);
    // Updates the hint startIdle stored, and the card when idle is up and no play is running.
    void setIdleHint(const std::string& hint);
    void setIdleName(const std::string& name);
    owenb_osd::AspectStats aspectStats() const { return aspect_.stats(); }
    void queueDepths(unsigned long long& out, unsigned long long& ready, unsigned long long& probe) const;
    owenb_osd::AspectEnd lastAspectEnd() const { return lastAspectEnd_.load(); }

private:
    WindowFactory windowFactory_;
    SourceFactory sourceFactory_;
    mutable std::mutex mu_;
    std::unique_ptr<OwenbDdrWindow> window_;
    std::unique_ptr<OwenbRing> ring_;
    std::unique_ptr<OwenbPlaySession> session_;
    std::thread thr_;
    std::atomic<bool> active_{false};
    int volume_ = 100;
    OsdConfig osdCfg_;
    owenb_osd::UiBus ui_;
    owenb_osd::RendererStats osdLast_;
    bool fbMapped_ = false;
    std::unique_ptr<RingOverlayPort> port_;
    std::unique_ptr<owenb_osd::OsdThread> osd_;
    owenb_osd::AspectGuard aspect_;
    AspectConfig aspectCfg_;
    std::atomic<int> pictureArm_{-1};
    std::atomic<int> pictureLive_{-1};
    std::atomic<int> spelunkrPicture_{-2};  // -2 no menu pick, -1 Auto, otherwise PictureMode
    std::atomic<bool> pictureDirty_{false};
    std::atomic<int> spelunkrAspect_{-1};  // -1 off, 1=4:3, 2=16:9
    void applyArmedPicture(int mode);
    void applySpelunkrAspectLocked();
    void armAspectAfterNewSessionLocked(int armedPictureMode);
    std::atomic<owenb_osd::AspectEnd> lastAspectEnd_{owenb_osd::AspectEnd::Nothing};
    std::atomic<bool> aspectEnded_{true};
    void endAspect(OwenbDdrWindow* w);
    // T7 (under mu_ unless atomic)
    std::function<void(const std::string&)> log_;
    IdleConfig idleCfg_;
    bool idleWanted_ = false, idleUp_ = false, idleFailLogged_ = false;
    owenb_osd::Raster idleRaster_{};   // FIELD-1018 F3: the raster the idle card was last armed on
    bool idleRasterValid_ = false;
    std::atomic<unsigned> idleRearms_{0};
    std::atomic<bool> idleRearmReq_{false};   // requestIdleRearm()
    std::atomic<unsigned> idleStarts_{0};
    std::atomic<bool> keeperStop_{false};
    std::thread keeper_;
    void logLocked(const std::string& s) const;
    bool openRingLocked(std::string& err);
    std::function<void()> beforeRingOpen_;
    std::function<void()> onOsdStarted_;
    std::function<void()> osdStartedFire_;   // filled under mu_, invoked after unlock
    void startOsdLocked();
    void fireOsdStarted();
    void teardownLocked();
    bool idleBeginLocked();
    void keeperLoop();
};

} // namespace misterplex::owenb
