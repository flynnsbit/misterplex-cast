#pragma once
// OWENB-P1B T6: the ARM overlay renderer (ARM-RENDERER-SPEC-1017 §1, §5, §6) -- host-testable.
//
//   UiBus      MediaPlayer's writes (show / skip / pause / seek / toast / chip / error / menu) under a
//              mutex + condvar + generation; resolve(now) applies the timers (§3, §6).
//   Renderer   one tick: read the core's status (word 46 ovl_*, 47 / 80 / 30 raster), lay out, draw
//              the back page, show it; never touch a page the core may still read: a page is
//              written again only after ovl_gen >= the generation that retired it (signed compare,
//              hps/lib/h264ring.c:823-842 h264ring_overlay_wait). Refused (ovl_state 2) -> log
//              ovl_err and hide. Double-buffered 8888 when fb0 allows (Decision 2); else one page:
//              hide, wait for the core, draw, show -- one blank frame a redraw (Decision 3), a
//              runtime path (PagePlan::buf == Single), never the default.
//   Aspect     (0006) moved out: owenb_aspect_guard.hpp saves the user's Analog Aspect, asks for
//              Letterbox (Decision 1) and restores the user's value at the session's end.
//   OsdThread  the loop: redraw only on a new content key, at most 8 Hz; 2 ms polls while a flip is
//              pending (§6).
// The pages are fb0 (h264ring_fb_map), never /dev/mem: 0x31600000 (the idle framebuffer) is not in
// [H264RING_FB_BYTE, + FB_SIZE) = [0x22000000, 0x22800000) and nothing here can address it.

#include "idle_raster_credible.hpp"
#include "owenb_osd_draw.hpp"
#include "pause_dim.hpp"
#include "spelunkr/banner.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace misterplex::owenb_osd {

// What the renderer reads from the core each tick.
struct OvlStatus {
    bool valid = false;
    uint32_t gen = 0;
    unsigned state = H264RING_OVL_S_OFF;
    unsigned err = 0;
    bool at = false;
    Raster raster;
    unsigned osdAnalogAspect = 0;
    unsigned osdAspect = 0;   // 0008: word 27 OSD_ASPECT, the menu's Aspect Ratio (0 Auto, 1 4:3, 2 16:9)
    unsigned osdVideoOut = 0; // word 27 OSD_VIDEO_OUT: 0 Auto, 1 Interlaced, 2 Progressive, 3 Native
    unsigned setSeq = 0;   // word 30 SET_SEQ: the last settings request consumed (h264ring_layout.h:414-416)
    unsigned setErr = 0;   // word 27 SET_ERR: its result, 0 applied (h264ring_layout.h:372-374)
    bool session = false;
};

// The daemon's h264ring handle, as the renderer uses it (OwenbRing's overlay methods: each takes
// the ring mutex for microseconds, §1). Tests implement it with a fake core.
class IOverlayPort {
public:
    virtual ~IOverlayPort() = default;
    virtual int show(const struct ::h264ring_overlay& o) = 0;   // generation > 0 or -errno
    virtual int hide() = 0;
    virtual OvlStatus status() = 0;
    virtual int requestSettings(unsigned mask, const unsigned values[H264RING_OPT_COUNT]) = 0;
    virtual int clearSettings() = 0;
};

// fb0 as h264ring_fb_map returned it (h264ring.h:318-330).
struct FbMap {
    uint8_t* virt = nullptr;
    uint32_t phys = 0;
    uint32_t len = 0;
};

inline bool genReached(uint32_t acked, uint32_t gen) { return int32_t(acked - gen) >= 0; }

// The timed UI state (§3): what MediaPlayer sets, with the instants things expire.
struct UiModel {
    static constexpr int64_t kBarMs = 4000;      // Playing: shown 4 s after a show / skip
    static constexpr int64_t kSettleMs = 500;    // the seek's ghost knob
    static constexpr int64_t kScrubThumbMs = 4000; // preview stays up long enough to see on 480i
    static constexpr int64_t kToastMs = 3000;
    static constexpr int64_t kChipMs = 5000;     // the spec gives no time for the chip (gaps G6)
    static constexpr int64_t kTransportMs = 1500;
    static constexpr int64_t kErrorMs = 15000;   // T7: an error screen, then back to the idle card (gaps G17)

    bool presented = false, paused = false, buffering = false;
    std::string serviceNotice;
    bool serviceFailed = false;
    int bufferingPct = -1;
    std::string statusText;     // NET_RESUME reconnect banner; empty keeps "BUFFERING..."
    int64_t positionMs = 0, durationMs = 0, bufferedMs = -1;
    std::string title;
    int64_t barUntilMs = 0;
    std::string transport;
    int64_t transportUntilMs = 0;
    int64_t seekTargetMs = -1, seekSettleUntilMs = 0;
    std::shared_ptr<const ScrubThumb> scrubThumb;
    std::string castController;
    int64_t chipUntilMs = 0;
    struct Toast {
        std::string text;
        int64_t untilMs;
    };
    std::deque<Toast> toasts;
    ErrorScreen error = ErrorScreen::None;
    std::string errorDetail;
    int64_t errorUntilMs = 0;   // T7: 0 = until cleared
    // Spelunkr TV chrome on this overlay (2 s opaque + 0.4 s fade, overlay clock).
    std::string spelunkrDial, spelunkrName, spelunkrTitle;
    bool spelunkrTuning = false;
    bool spelunkr = false;
    int64_t bannerShowUntilMs = 0;
    int64_t bannerGoneMs = 0;
    void clearSpelunkrBannerFields() {
        spelunkrTuning = false;
        spelunkrDial.clear();
        spelunkrName.clear();
        spelunkrTitle.clear();
        bannerShowUntilMs = 0;
        bannerGoneMs = 0;
    }
    // First picture starts the 2 s overlay-clock banner hide.
    // Clearing the banner here left it up forever when the clear missed.
    void armSpelunkrBannerHide(int64_t now) {
        spelunkrTuning = false;
        if (spelunkrDial.empty()) {
            bannerShowUntilMs = 0;
            bannerGoneMs = 0;
            return;
        }
        if (spelunkrTitle.empty() && !title.empty())
            spelunkrTitle = title;
        bannerShowUntilMs = now + misterplex::spelunkr::kSpelunkrBannerMs;
        bannerGoneMs = now + misterplex::spelunkr::kSpelunkrBannerMs + misterplex::spelunkr::kSpelunkrBannerFadeMs;
    }
    bool spelunkrGuideOpen = false;
    int spelunkrGuideCursor = -1;
    std::vector<LibraryRow> spelunkrGuideRows;
    Menu menu;
    bool idle = false;          // T7: the idle session (no play): the idle card
    // PLAY-PENDING: accepted play/cast, waiting for the first HDMI picture.
    // The big chevron is up; the status card and PlexUI surface stay off.
    // pendingLabel is the source line ("Loading YouTube..."). Empty keeps the chevron alone.
    bool pending = false;
    uint64_t pendingGen = 0;    // play generation that armed the cue (0 none)
    std::string pendingLabel;
    std::string idleHint, idleName;
    // AMBIENT deck. Empty unless the flag is on and a fetch published cards.
    std::shared_ptr<const misterplex::AmbientDeck> ambient;
    int64_t ambientEpochMs = 0;
    LibraryPage library;        // 0010: the daemon's browser, published by MediaPlayer (obLibraryPublish)
    std::shared_ptr<const FullSurface> surface;   // OB port: PlexUI's frame (null: closed)
    std::string caption1, caption2;
    bool captionArmed = false;
    int captionCue = -1;
    int64_t captionStreamId = 0;
    int64_t captionMediaMs = 0;
    MarkerOverlay marker;
    OverscanView overscan;
    // Playback ended (UI stop, cast stop, EOF, error, idle): no caption text and no reserved caption band stays.
    void clearCaptions() {
        caption1.clear();
        caption2.clear();
        captionArmed = false;
        captionCue = -1;
        captionStreamId = 0;
        captionMediaMs = 0;
    }

    UiState resolve(int64_t now) const {
        auto applySpelunkrChrome = [&](UiState &u) {
            u.spelunkr = spelunkr;
            // GH#410: a hide deadline always wins. Stuck spelunkrTuning used to keep
            // the bar opaque after first picture if the stamp ran but tuning stuck.
            const bool timed = bannerGoneMs > 0;
            const bool show = !spelunkrDial.empty() && (timed ? now < bannerGoneMs : spelunkrTuning);
            if (show) {
                u.spelunkrBanner.on = true;
                u.spelunkrBanner.dial = spelunkrDial;
                u.spelunkrBanner.name = spelunkrName;
                u.spelunkrBanner.title = spelunkrTitle;
                if (!timed || now < bannerShowUntilMs)
                    u.spelunkrBanner.alpha = 255;
                else {
                    const int64_t fade = bannerGoneMs - bannerShowUntilMs;
                    const int64_t left = bannerGoneMs - now;
                    u.spelunkrBanner.alpha = fade > 0 ? uint8_t(std::max<int64_t>(0, left * 255 / fade)) : 0;
                }
            }
            u.spelunkrGuide.open = spelunkrGuideOpen && !menu.open && !overscan.open;
            u.spelunkrGuide.cursor = spelunkrGuideCursor;
            u.spelunkrGuide.rows = spelunkrGuideRows;
            if (u.spelunkrGuide.open || menu.open || overscan.open || library.open ||
                (error != ErrorScreen::None && (errorUntilMs == 0 || now < errorUntilMs)))
                u.spelunkrBanner = {};  // GH#410: other chrome; do not stack the 2 s bar
        };
        if (!serviceNotice.empty()) {
            UiState s;
            if (serviceFailed) {
                s.error = ErrorScreen::StreamError;
                s.errorDetail = serviceNotice;
            } else {
                s.pending = true;
                s.pendingLabel = serviceNotice;
            }
            applySpelunkrChrome(s);
            return s;
        }
        if (overscan.open) {
            UiState o;
            o.overscan = overscan;
            applySpelunkrChrome(o);
            return o;
        }
        UiState s;
        s.title = title;
        s.positionMs = positionMs;
        s.durationMs = durationMs;
        s.bufferedMs = bufferedMs;
        s.bufferingPct = bufferingPct;
        s.statusText = statusText;
        s.menu = menu;
        if (error != ErrorScreen::None && (errorUntilMs == 0 || now < errorUntilMs)) {
            s.error = error;
            s.errorDetail = errorDetail;
            applySpelunkrChrome(s);
            return s;
        }
        // Chevron only, until the first-picture callback clears pending. A previous
        // play's presented flag (and buffering) must not hide this cue or replace it
        // with the thin BUFFERING bar.
        if (pending) {
            UiState p;
            p.pending = !spelunkrTuning;
            p.pendingLabel = pendingLabel;
            p.spelunkrStatic = false;
            applySpelunkrChrome(p);
            return p;
        }
        // GH#410: Spelunkr chrome is the channel banner. The Plex 4 s play bar
        // outlasts the 2 s hide and looks like the banner never went away.
        if (!spelunkr) {
            if (buffering)
                s.bar = Bar::Buffering;
            else if (now < seekSettleUntilMs)
                s.bar = Bar::Seeking;
            else if (paused)
                s.bar = Bar::Paused;   // pinned
            else if (presented && now < barUntilMs)
                s.bar = Bar::Playing;
        }
        if (!spelunkr && now < seekSettleUntilMs) {
            s.seekTargetMs = seekTargetMs;
            if (scrubThumb && scrubThumb->visible())
                s.scrubThumb = scrubThumb;
        }
        if (!spelunkr && now < transportUntilMs)
            s.transport = transport;
        if (!spelunkr && now < chipUntilMs)
            s.chip = castController;
        if (idle && !presented && !buffering) {
            s.idle = true;
            s.idleHint = idleHint;
            s.idleName = idleName;
        }
        s.library = library;
        if (!s.idle) {
            s.caption1 = caption1;
            s.caption2 = caption2;
            s.captionArmed = captionArmed;
            s.captionCue = captionCue;
            s.captionStreamId = captionStreamId;
            s.captionMediaMs = captionMediaMs;
            s.marker = marker;
        }
        if (surface && !presented && !buffering)   // OB port: PlexUI over idle; never over a play
            s.surface = surface;
        if (s.idle && !s.surface && !s.menu.open && !s.library.open && error == ErrorScreen::None && ambient &&
            misterplex::ambientEnabled()) {
            const int cards = misterplex::ambientVisibleCount(*ambient);
            const int slot = misterplex::ambientSlot(now, ambientEpochMs, cards);
            if (slot >= 0) {
                s.ambient = ambient;
                s.ambientSlot = slot;
            }
        }
        for (const auto& t : toasts)
            if (now < t.untilMs)
                s.toasts.push_back(t.text);
        while (s.toasts.size() > 2)
            s.toasts.erase(s.toasts.begin());
        applySpelunkrChrome(s);
        return s;
    }

    // the next instant resolve() changes on its own (INT64_MAX: none)
    int64_t nextChange(int64_t now) const {
        int64_t n = INT64_MAX;
        // GH#410: Plex bar/skip timers must not own the OSD wake while Spelunkr
        // chrome is the channel banner.
        if (!spelunkr) {
            for (int64_t t : {barUntilMs, transportUntilMs, seekSettleUntilMs, chipUntilMs})
                if (t > now)
                    n = std::min(n, t);
        }
        for (int64_t t : {errorUntilMs, bannerShowUntilMs, bannerGoneMs})
            if (t > now)
                n = std::min(n, t);
        for (const auto& t : toasts)
            if (t.untilMs > now)
                n = std::min(n, t.untilMs);
        if (ambient && idle && !presented && !pending && !buffering && !menu.open && !library.open &&
            error == ErrorScreen::None && !surface && misterplex::ambientEnabled()) {
            const int cards = misterplex::ambientVisibleCount(*ambient);
            const int64_t at = misterplex::ambientNextChange(now, ambientEpochMs, cards);
            if (at > now)
                n = std::min(n, at);
        }
        return n;
    }
};

// MediaPlayer -> renderer (§6): every write bumps gen and wakes the renderer.
class UiBus {
public:
    void setServiceNotice(const std::string& text, bool failed = false) {
        {
            std::lock_guard<std::mutex> g(mu_);
            serviceNotice_ = text;
            serviceFailed_ = failed;
            serviceShown_.clear();
            ++gen_;
        }
        cv_.notify_all();
    }
    void noteServiceShown(const std::string& text) {
        std::lock_guard<std::mutex> g(mu_);
        if (text == serviceNotice_)
            serviceShown_ = text;
    }
    bool serviceNoticeShown(const std::string& text) const {
        std::lock_guard<std::mutex> g(mu_);
        return !text.empty() && serviceShown_ == text;
    }
    template <class F>
    void update(F&& f) {
        {
            std::lock_guard<std::mutex> g(mu_);
            f(m_);
            ++gen_;
        }
        cv_.notify_all();
    }
    void showBar(int64_t now) { update([&](UiModel& m) { m.barUntilMs = now + UiModel::kBarMs; }); }
    void setSpelunkrMode(bool on) {
        update([=](UiModel& m) {
            m.spelunkr = on;
            if (on) {
                m.idleName = "Spelunkr";
                m.idleHint = "Open the Guide";
                if (!m.presented && !m.pending)
                    m.idle = true;
            }
        });
    }
    void showSpelunkrLanding(const std::string& detail = {}) {
        update([&](UiModel& m) {
            m.spelunkr = true;
            const auto dial = m.spelunkrDial;
            const auto name = m.spelunkrName;
            m.clearSpelunkrBannerFields();
            if (!detail.empty()) {
                m.spelunkrDial = dial.empty() ? "Spelunkr" : dial;
                m.spelunkrName = name;
                m.spelunkrTitle = detail;
                m.spelunkrTuning = true;
            }
            m.spelunkrGuideOpen = false;
            m.pending = false;
            m.pendingGen = 0;
            m.pendingLabel.clear();
            m.error = ErrorScreen::None;
            m.errorDetail.clear();
            m.errorUntilMs = 0;
            m.surface.reset();
            m.idle = true;
            m.idleName = "Spelunkr";
            m.idleHint = "Open the Guide";
            m.presented = m.paused = m.buffering = false;
            m.barUntilMs = 0;
            m.clearCaptions();
        });
    }
    void flashTransport(int64_t now, const std::string& text) {
        update([&](UiModel& m) {
            m.transport = text;
            m.transportUntilMs = now + UiModel::kTransportMs;
            m.barUntilMs = now + UiModel::kBarMs;
        });
    }
    void setPlayback(bool presented, bool paused, int64_t positionMs, int64_t durationMs) {
        update([&](UiModel& m) {
            m.presented = presented;
            m.paused = paused;
            m.positionMs = positionMs;
            m.durationMs = durationMs;
        });
    }
    void setTitle(const std::string& t) { update([&](UiModel& m) { m.title = t; }); }
    void setBuffering(bool on, int pct = -1) {
        update([&](UiModel& m) {
            m.buffering = on;
            m.bufferingPct = pct;
            if (!on)
                m.statusText.clear();
        });
    }
    // Accept: card and PlexUI off, big chevron until the first picture.
    // label is the source line (pendingLoadingLine). Empty keeps the chevron alone.
    void armPlayPending(uint64_t gen, const std::string &label = {}, const std::string &dial = {},
                        const std::string &name = {}) {
        update([=](UiModel &m) {
            m.pending = true;
            m.pendingGen = gen;
            m.pendingLabel = label;
            m.idle = false;
            m.presented = false;
            m.surface.reset();
            m.error = ErrorScreen::None;
            m.errorDetail.clear();
            m.errorUntilMs = 0;
            m.buffering = false;
            m.clearSpelunkrBannerFields();
            m.spelunkrTuning = !dial.empty();
            m.spelunkrDial = dial;
            m.spelunkrName = name;
        });
    }
    // First HDMI picture of this generation. A newer accept's gen is left alone.
    // GH#410: pass overlay-clock `now` so Spelunkr starts the 2 s hide instead of dropping the bar.
    void clearPlayPending(uint64_t gen, int64_t now = 0) {
        update([gen, now](UiModel& m) {
            if (m.pendingGen != 0 && gen != m.pendingGen)
                return;
            m.pending = false;
            m.pendingGen = 0;
            m.pendingLabel.clear();
            if (m.spelunkrTuning) {
                if (now > 0)
                    m.armSpelunkrBannerHide(now);
                else
                    m.clearSpelunkrBannerFields();
            }
        });
    }
    void failSpelunkrTune(const std::string& detail) {
        update([&](UiModel& m) {
            m.pending = false;
            m.pendingLabel.clear();
            m.buffering = false;
            m.error = ErrorScreen::None;
            m.errorDetail.clear();
            m.spelunkrTuning = true;
            m.spelunkrTitle = detail;
            m.bannerShowUntilMs = 0;
            m.bannerGoneMs = 0;
        });
    }
    bool pending() const {
        std::lock_guard<std::mutex> g(mu_);
        return m_.pending;
    }
    void setBuffered(int64_t ms) { update([&](UiModel& m) { m.bufferedMs = ms; }); }
    void seek(int64_t now, int64_t targetMs) {
        update([&](UiModel& m) {
            m.seekTargetMs = targetMs;
            m.seekSettleUntilMs = now + UiModel::kSettleMs;
            m.barUntilMs = now + UiModel::kBarMs;
            m.scrubThumb.reset();
        });
    }
    // A decoded BIF frame. Extends the seek settle so the preview stays on the
    // glass long enough to read. No frame leaves the 500 ms ghost knob.
    void setScrubThumb(int64_t now, std::shared_ptr<const ScrubThumb> thumb) {
        update([now, thumb](UiModel& m) {
            m.scrubThumb = thumb;
            if (!m.scrubThumb || !m.scrubThumb->visible())
                return;
            const int64_t until = now + UiModel::kScrubThumbMs;
            if (m.seekSettleUntilMs < until)
                m.seekSettleUntilMs = until;
            if (m.barUntilMs < until)
                m.barUntilMs = until;
        });
    }
    void showSpelunkrBanner(int64_t now, const std::string& dial, const std::string& name, const std::string& title,
                           bool tuning = false) {
        update([=](UiModel& m) {
            m.spelunkrDial = dial;
            m.spelunkrName = name;
            m.spelunkrTitle = title;
            // P0 11-10 zap guide flash: a banner stamped while a zap is still pending (skip/supersede
            // notice, OSD-start replay) must keep the tune armed, or the expired banner drops resolve()
            // onto the "OPEN THE GUIDE" landing over the held picture until the next skip re-stamps it.
            m.spelunkrTuning = tuning || m.pending; // Keep MBB chevron suppressed; deadline still owns the bar.
            m.bannerShowUntilMs = now + misterplex::spelunkr::kSpelunkrBannerMs;
            m.bannerGoneMs = now + misterplex::spelunkr::kSpelunkrBannerMs + misterplex::spelunkr::kSpelunkrBannerFadeMs;
        });
    }
    // GH#410: first picture missed the stamp; start hide now that pending is down.
    void healSpelunkrBannerHide(int64_t now) {
        update([now](UiModel& m) {
            if (m.spelunkrTuning && !m.idle && !m.pending && m.bannerGoneMs == 0 && !m.spelunkrDial.empty())
                m.armSpelunkrBannerHide(now);
        });
    }
    // GH#410: fill now-playing without restamping the 2 s hide.
    void fillSpelunkrBannerTitle(const std::string& title, const std::string& name = {}) {
        update([=](UiModel& m) {
            if (m.spelunkrDial.empty())
                return;
            if (!name.empty())
                m.spelunkrName = name;
            if (!title.empty())
                m.spelunkrTitle = title;
        });
    }
    void clearSpelunkrBanner() {
        update([](UiModel& m) { m.clearSpelunkrBannerFields(); });
    }
    void setSpelunkrGuide(bool open, int cursor, std::vector<LibraryRow> rows) {
        update([=](UiModel& m) mutable {
            m.spelunkrGuideOpen = open;
            m.spelunkrGuideCursor = cursor;
            m.spelunkrGuideRows = std::move(rows);
        });
    }
    void toast(int64_t now, const std::string& text) {
        update([&](UiModel& m) {
            m.toasts.push_back({text, now + UiModel::kToastMs});
            while (m.toasts.size() > 2)   // at most 2 queued (§3): the oldest goes
                m.toasts.pop_front();
        });
    }
    void castFrom(int64_t now, const std::string& controller) {
        update([&](UiModel& m) {
            m.castController = controller;
            m.chipUntilMs = controller.empty() ? 0 : now + UiModel::kChipMs;
        });
    }
    void setError(ErrorScreen e, const std::string& detail) {
        update([&](UiModel& m) {
            m.error = e;
            m.errorDetail = detail;
            m.errorUntilMs = 0;
            if (e != ErrorScreen::None) {
                if (m.spelunkrTuning) {
                    m.pending = false;
                    m.pendingGen = 0;
                    m.pendingLabel.clear();
                }
                m.clearSpelunkrBannerFields();
            }
        });
    }
    // T7: an error screen for `ms` (then the idle card), 0 = until cleared
    void setErrorFor(int64_t now, ErrorScreen e, const std::string& detail, int64_t ms = UiModel::kErrorMs) {
        update([&](UiModel& m) {
            m.error = e;
            m.errorDetail = detail;
            m.errorUntilMs = ms > 0 ? now + ms : 0;
            // GH#410: error must not resurrect the channel banner after it ends.
            if (e != ErrorScreen::None) {
                if (m.spelunkrTuning) {
                    m.pending = false;
                    m.pendingGen = 0;
                    m.pendingLabel.clear();
                }
                m.clearSpelunkrBannerFields();
            }
        });
    }
    void clearError() { setError(ErrorScreen::None, ""); }
    // T7: the idle session's card (on: no play; off: a play starts)
    // Publish the idle art deck. A new deck starts the cycle at `nowMs`.
    // The same deck (a re-arm) keeps the epoch so the card does not jump.
    void setAmbient(std::shared_ptr<const misterplex::AmbientDeck> deck, int64_t nowMs) {
        update([deck, nowMs](UiModel& m) {
            if (m.ambient.get() != deck.get())
                m.ambientEpochMs = nowMs;
            m.ambient = deck;
        });
    }
    void setIdle(bool on, const std::string& name = {}, const std::string& hint = {}) {
        update([&](UiModel &m) {
            if (on && m.pending && m.spelunkrTuning)
                return; // A channel handoff must not resurrect the landing card.
            // GH#27: startIdle / raster re-arm must not resurrect Ready over plexui.
            if (on && m.surface)
                return;
            m.idle = on;
            if (on) {
                // Stop / open-fail / nothing queued: IDLE card, no big watermark.
                if (m.pending)
                    m.surface.reset();
                m.pending = false;
                m.pendingGen = 0;
                m.pendingLabel.clear();
                m.idleName = name;
                m.idleHint = hint;
                if (m.spelunkr && m.idleHint.empty())
                    m.idleHint = "Open the Guide";  // GH#411: stop() must not flash Ready to cast
                m.presented = m.paused = m.buffering = false;
                m.barUntilMs = 0;
                m.marker.clear();
                // G11: Stop → Ready. Captions must not stay on the idle chevron.
                m.clearCaptions();
            }
        });
    }
    void setMenu(const Menu& menu) { update([&](UiModel& m) { m.menu = menu; }); }
    void setOverscan(const OverscanView& v) { update([&](UiModel& m) { m.overscan = v; }); }
    // 0010: the library page (open = false hides it)
    void setLibrary(const LibraryPage& page) { update([&](UiModel& m) { m.library = page; }); }
    // OB port: PlexUI's full-screen frame; null closes it (the idle card / OSD come back)
    void setSurface(std::shared_ptr<const FullSurface> f) {
        update([&](UiModel& m) {
            // PlexUI must not paint over the play-pending chevron.
            if (m.pending) {
                m.surface.reset();
                return;
            }
            m.surface = std::move(f);
        });
    }
    // OB port: the canvas the renderer lays out on (the core's raster), for PlexUI to size its frame
    void noteCanvas(unsigned w, unsigned h) {
        canvasW_.store(w);
        canvasH_.store(h);
    }
    unsigned canvasW() const { return canvasW_.load(); }
    unsigned canvasH() const { return canvasH_.load(); }
    void reset() { update([&](UiModel& m) { m = UiModel{}; }); }

    UiModel snapshot(uint64_t* gen = nullptr) const {
        std::lock_guard<std::mutex> g(mu_);
        if (gen)
            *gen = gen_;
        UiModel copy = m_;
        copy.serviceNotice = serviceNotice_;
        copy.serviceFailed = serviceFailed_;
        return copy;
    }
    // wait until gen moves past `seen`, `stop` turns true, or `ms` pass
    void waitFor(uint64_t seen, int64_t ms, const std::function<bool()>& stop) {
        std::unique_lock<std::mutex> g(mu_);
        cv_.wait_for(g, std::chrono::milliseconds(std::max<int64_t>(0, ms)),
                     [&] { return gen_ != seen || (stop && stop()); });
    }
    void wake() {
        { std::lock_guard<std::mutex> g(mu_); }
        cv_.notify_all();
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    UiModel m_;
    std::string serviceNotice_;
    std::string serviceShown_;
    bool serviceFailed_ = false;
    uint64_t gen_ = 0;
    std::atomic<unsigned> canvasW_{0}, canvasH_{0};
};

struct RendererStats {
    unsigned shows = 0, hides = 0, refused = 0, timeouts = 0, replans = 0, blankFrames = 0;
    unsigned draws = 0, skippedSameKey = 0, noPage = 0;
    unsigned lastErr = 0;
    // OB port: the PlexUI surface path
    unsigned surfDraws = 0, surfInPlace = 0, surfTooBig = 0;
    uint64_t surfRows = 0, surfBytes = 0;
    int64_t surfLastWriteUs = 0, surfMaxWriteUs = 0;
};

class Renderer {
public:
    static constexpr int64_t kMinRedrawMs = 125;   // 8 Hz (§6)
    static constexpr int64_t kPollMs = 2;          // while a flip is pending (§6)
    static constexpr int64_t kAckTimeoutMs = 500;
    static constexpr int64_t kSurfaceMinMs = 16;   // OB port: PlexUI nav, not the 8 Hz OSD cap

    Renderer(IOverlayPort& port, FbMap fb, bool allowDouble = true,
             std::function<void(const std::string&)> log = {})
        : port_(port), fb_(fb), allowDouble_(allowDouble), log_(std::move(log)) {}

    // One step; returns how long until the next is due (ms).
    int64_t tick(int64_t now, const UiState& ui);

    // The end of the session: hide (and give the core up to `ms` to take it).
    void shutdown(const std::function<void(unsigned)>& sleepMs, unsigned ms = 100);

    const RendererStats& stats() const { return st_; }
    const PagePlan& plan() const { return plan_; }
    const Layout& layout() const { return layout_; }
    int shownPage() const { return shown_; }
    bool pending() const { return pending_; }
    bool showing(const UiState& ui) const {
        return layoutValid_ && shown_ >= 0 && !pending_ && !refusedHold_ &&
               contentKey(ui, layout_) == shownKey_;
    }
    const Surface& surface() const { return surf_; }
    bool surfaceMode() const { return surfMode_; }
    // OB port: called with a PlexUI frame's gen once the core shows it (double: ovl_gen reached;
    // single in-place: the rows are written). Renderer thread.
    void setOnSurfaceShown(std::function<void(uint64_t)> f) { onSurfaceShown_ = std::move(f); }

private:
    void logf(const std::string& s) {
        if (log_)
            log_(s);
    }
    void notePauseDim(const UiState& ui, int64_t now);
    bool pageFree(int i, uint32_t acked) const {
#ifdef MUT_OSD_REUSE_BEFORE_GEN
        (void)acked;
        return i != shown_;   // mutant: reuse the retired page before the core moved past it
#else
        return !busy_[i] || (i != shown_ && retiredValid_[i] && genReached(acked, retiredAt_[i]));
#endif
    }
    void retireShown(uint32_t gen) {
        if (shown_ >= 0) {
            retiredAt_[shown_] = gen;
            retiredValid_[shown_] = true;
        }
    }
    void startPending(int gen, int64_t now, bool isHide) {
        pending_ = true;
        pendingGen_ = uint32_t(gen);
        pendingSince_ = now;
        pendingHide_ = isHide;
        timeoutLogged_ = false;
        if (!isHide)
            pendingScreen_ = frameScreen_;
    }
    // The core never took this flip. Drop it without writing the page the core
    // is still reading, so the new screen (the idle card) can use the other page.
    void abandonUntaken() {
        const uint32_t gen = pendingGen_;
        const bool wasHide = pendingHide_;
        const int written = shown_;
        pending_ = false;
        pendingHide_ = false;
        int live = -1;
        for (int i = 0; i < 2; ++i) {
            if (busy_[i] && retiredValid_[i] && retiredAt_[i] == gen)
                live = i;
        }
        if (wasHide) {
            if (live >= 0) {
                shown_ = live;
                retiredValid_[live] = false;
                shownKey_ = 0;
            }
            return;
        }
        if (written >= 0 && written != live) {
            busy_[written] = false;
            retiredValid_[written] = false;
        }
        if (live >= 0) {
            shown_ = live;
            retiredValid_[live] = false;
            shownKey_ = 0;
        } else {
            shown_ = -1;
            shownKey_ = 0;
        }
    }
    int64_t doHide(int64_t now);
    int64_t drawAndShow(int64_t now, const UiState& ui, const Layout& L, uint64_t key, uint32_t acked);
    int64_t tickSurface(int64_t now, const FullSurface& S, const Layout& L, uint32_t acked);
    uint64_t writeDirtyRows(int page, const FullSurface& S, uint32_t stride);

    IOverlayPort& port_;
    FbMap fb_;
    bool allowDouble_;
    std::function<void(const std::string&)> log_;
    RendererStats st_;
    Layout layout_;
    bool layoutValid_ = false;
    PagePlan plan_;
    Surface surf_;
    int shown_ = -1;              // the page the core shows (or will at pendingGen_), -1 none
    uint64_t shownKey_ = 0;
    bool busy_[2] = {false, false};
    bool retiredValid_[2] = {false, false};
    uint32_t retiredAt_[2] = {0, 0};
    bool pending_ = false, pendingHide_ = false, timeoutLogged_ = false;
    uint64_t frameScreen_ = 0, pendingScreen_ = 0;
    uint32_t pendingGen_ = 0;
    int64_t pendingSince_ = 0;
    int64_t lastDrawMs_ = INT64_MIN / 2;
    bool noPageLogged_ = false, refusedHold_ = false;
    // OB port: PlexUI surface
    bool surfMode_ = false, surfSizeLogged_ = false, surfReplaceLive_ = false;
    unsigned lastCredibleW_ = 1280, lastCredibleH_ = 720;
    bool tinySubLogged_ = false;
    uint64_t surfGen_ = 0, surfPendingGen_ = 0;
    std::vector<uint8_t> shadow_[2];
    bool shadowValid_[2] = {false, false};
    std::function<void(uint64_t)> onSurfaceShown_;
    // PAUSE_DIM. 0 is full brightness. Flag off keeps both at 0, so the
    // same-key / same-gen skips stay on the tip path.
    int wantDim_ = 0;
    int drawnDim_ = 0;
    PauseDimClock dimClock_{};
};

inline void Renderer::notePauseDim(const UiState& ui, int64_t now) {
    wantDim_ = 0;
    if (!pauseDimEnabled()) {
        dimClock_ = {};
        return;
    }
    PauseDimInput in;
    in.paused = ui.bar == Bar::Paused;
    in.staticUi = pauseDimIsStatic(ui.surface != nullptr, ui.library.open, ui.menu.open, ui.idle, ui.pending);
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    mix(ui.surface ? ui.surface->gen : 0);
    mix(ui.library.open ? 1u : 0u);
    mix(static_cast<uint32_t>(ui.library.cursor));
    mix(static_cast<uint32_t>(ui.library.offset));
    for (unsigned char c : ui.library.header)
        mix(c);
    mix(ui.menu.open ? 1u : 0u);
    mix(static_cast<uint32_t>(ui.menu.selected));
    in.stillKey = h;
    in.nowMs = now;
    wantDim_ = pauseDimWant(in, dimClock_);
}

inline int64_t Renderer::doHide(int64_t now) {
    const int g = port_.hide();
    if (g <= 0) {
        logf("overlay: hide failed (" + std::to_string(g) + ")");
        return kMinRedrawMs;
    }
    ++st_.hides;
    retireShown(uint32_t(g));
    shown_ = -1;
    shownKey_ = 0;
    startPending(g, now, true);
    return kPollMs;
}

inline void Renderer::shutdown(const std::function<void(unsigned)>& sleepMs, unsigned ms) {
    if (shown_ >= 0 || pending_) {
        const int g = port_.hide();
        if (g > 0) {
            ++st_.hides;
            for (unsigned t = 0; t < ms; t += 2) {
                const OvlStatus s = port_.status();
                if (!s.valid || genReached(s.gen, uint32_t(g)))
                    break;
                if (sleepMs)
                    sleepMs(2);
            }
        }
    }
    shown_ = -1;
    shownKey_ = 0;
    pending_ = false;
    busy_[0] = busy_[1] = false;
}

inline int64_t Renderer::tick(int64_t now, const UiState& ui) {
    notePauseDim(ui, now);
    const OvlStatus s = port_.status();
    if (!s.valid)
        return kMinRedrawMs;
    // GH#14: core status can report 256x144 while HDMI is 720p. Skipping the
    // whole tick hid plexui (black glass). Layout on the last credible raster
    // instead of re-arming Ready on the tiny one.
    Raster layoutRaster = s.raster;
    if (!misterplex::owenb::idleRasterCredible(layoutRaster.w, layoutRaster.h)) {
        layoutRaster.w = lastCredibleW_;
        layoutRaster.h = lastCredibleH_;
        if (!tinySubLogged_) {
            tinySubLogged_ = true;
            logf("overlay: tiny raster " + std::to_string(s.raster.w) + "x" +
                 std::to_string(s.raster.h) + "; layout " + std::to_string(layoutRaster.w) + "x" +
                 std::to_string(layoutRaster.h) + " (GH#14)");
        }
    } else {
        lastCredibleW_ = layoutRaster.w;
        lastCredibleH_ = layoutRaster.h;
        tinySubLogged_ = false;
    }
    frameScreen_ = screenKey(ui, layoutForFb(layoutRaster, fb_.len));
    if (pending_) {
#ifdef MUT_OSD_REUSE_BEFORE_GEN
        const bool reached = true;   // mutant: never wait for ovl_gen
#else
        const bool reached = genReached(s.gen, pendingGen_);
#endif
        if (!reached) {
            if (!timeoutLogged_ && now - pendingSince_ >= kAckTimeoutMs) {
                timeoutLogged_ = true;
                ++st_.timeouts;
                logf("overlay: generation " + std::to_string(pendingGen_) + " not taken in " +
                     std::to_string(kAckTimeoutMs) + " ms (ovl_gen " + std::to_string(s.gen) +
                     "); waiting, the retired page stays untouched");
            }
            // Same screen (a paused bar whose clock ticked): keep the one flip
            // outstanding and never write the retired page. A new screen (the
            // idle card after the final stop) is drawn on the page the core
            // did not take, once the wait has been logged.
            // GH#410: a hide that never acks must retry. pendingHide + empty
            // chrome is not a "new screen", and the channel bar would stick.
            if (pendingHide_ && timeoutLogged_ && !ui.anyVisible()) {
                pending_ = false;
                pendingHide_ = false;
                logf("overlay: hide generation " + std::to_string(pendingGen_) +
                     " not taken; retrying hide so the channel banner cannot stick");
                return doHide(now);
            }
            if (overlayWaitForAck(pendingHide_, ui.anyVisible(), frameScreen_ != pendingScreen_, timeoutLogged_))
                return kPollMs;
            logf("overlay: generation " + std::to_string(pendingGen_) +
                 " not taken; drawing the new screen, the retired page stays untouched");
            abandonUntaken();
        } else {
        pending_ = false;
        if (!pendingHide_ && surfPendingGen_ && s.state != H264RING_OVL_S_REFUSED && onSurfaceShown_)
            onSurfaceShown_(surfPendingGen_);
        surfPendingGen_ = 0;
        if (!pendingHide_ && s.state == H264RING_OVL_S_REFUSED) {
            ++st_.refused;
            st_.lastErr = s.err;
            logf("overlay: descriptor refused, ovl_err " + std::to_string(s.err) + "; hiding");
            refusedHold_ = true;   // until the layout changes: no retry loop
            return doHide(now);
        }
        }
    }
    for (int i = 0; i < 2; ++i)
        if (busy_[i] && i != shown_ && retiredValid_[i] && genReached(s.gen, retiredAt_[i])) {
            busy_[i] = false;
            retiredValid_[i] = false;
        }
    const Layout L = layoutForFb(layoutRaster, fb_.len);
    if (!layoutValid_ || L != layout_) {
        if (shown_ >= 0) {   // hide, wait for the core, lay out again (§1)
            ++st_.replans;
            return doHide(now);
        }
        layout_ = L;
        layoutValid_ = true;
        // bug 5: the raster as the core reports it, once per layout (glass: Auto / Interlaced vs Progressive)
        logf(std::string("overlay: layout ") + modeName(L.mode) + " " + std::to_string(L.W) + "x" +
             std::to_string(L.H) + " (raster " + std::to_string(s.raster.w) + "x" + std::to_string(s.raster.h) +
             (s.raster.interlaced ? " il" : "") + " vout " + std::to_string(s.raster.videoOut) + " ar " +
             std::to_string(s.raster.arX) + ":" + std::to_string(s.raster.arY) + ")");
        plan_ = PagePlan{};
        refusedHold_ = false;
    }
    if (ui.surface && !ui.overscan.open && !refusedHold_)
        return tickSurface(now, *ui.surface, L, s.gen);
    if (surfMode_) {   // PlexUI closed: off, then the OSD's own plan (idle card / bar) again
        surfMode_ = false;
        surfReplaceLive_ = false;
        surfGen_ = 0;
        plan_ = PagePlan{};
        if (shown_ >= 0)
            return doHide(now);
    }
    if (!ui.anyVisible())
        return shown_ >= 0 ? doHide(now) : kMinRedrawMs;
    if (refusedHold_)
        return kMinRedrawMs;
    const uint64_t key = contentKey(ui, L);
    if (shown_ >= 0 && key == shownKey_ && wantDim_ == drawnDim_) {
        ++st_.skippedSameKey;
        return kMinRedrawMs;
    }
    if (now - lastDrawMs_ < kMinRedrawMs)
        return lastDrawMs_ + kMinRedrawMs - now;
    return drawAndShow(now, ui, L, key, s.gen);
}

inline int64_t Renderer::drawAndShow(int64_t now, const UiState& ui, const Layout& L, uint64_t key, uint32_t acked) {
    const Frame f = planFrame(ui, L);
    int first, last;
    f.band(L, first, last);
    if (last < 0)
        return shown_ >= 0 ? doHide(now) : kMinRedrawMs;
    const PagePlan np = planPages(fb_.phys, fb_.len, L.W, L.H, unsigned(first), unsigned(last), allowDouble_);
    if (np.buf == Buffering::None) {
        ++st_.noPage;
        if (!noPageLogged_)
            logf("overlay: no page fits fb0 (" + std::to_string(fb_.len) + " bytes) for " + std::to_string(L.W) +
                 "x" + std::to_string(L.H));
        noPageLogged_ = true;
        return shown_ >= 0 ? doHide(now) : kMinRedrawMs;
    }
    const bool same = plan_.buf == np.buf && plan_.compact == np.compact && plan_.W == np.W && plan_.H == np.H &&
                      plan_.stride == np.stride && plan_.base[0] == np.base[0] && plan_.base[1] == np.base[1];
    if (!same && shown_ >= 0) {   // the pages move: off first
        ++st_.replans;
        return doHide(now);
    }
    if (!same && plan_.buf != np.buf)
        logf(std::string("overlay: ") + bufferingName(np.buf) + "-buffered 8888 " + std::to_string(L.W) + "x" +
             std::to_string(L.H) + (np.compact ? " (compact)" : "") + " in fb0 of " + std::to_string(fb_.len));
    plan_ = np;
    int target = 0;
    if (np.buf == Buffering::Double) {
        target = shown_ < 0 ? 0 : 1 - shown_;
        if (!pageFree(target, acked)) {
            if (shown_ < 0 && pageFree(1 - target, acked))
                target = 1 - target;
            else
                return kPollMs;
        }
    } else {
#ifndef MUT_OSD_SINGLE_NO_BLANK
        if (shown_ >= 0) {   // one page: off (a blank frame), then draw (Decision 3)
            ++st_.blankFrames;
            return doHide(now);
        }
#endif
        if (shown_ < 0 && !pageFree(0, acked))
            return kPollMs;
    }
    paint(surf_, ui, L, f);
    if (wantDim_ > 0)
        pauseDimApply(surf_.px.data(), surf_.px.size(), wantDim_);
    writeBand(fb_.virt + (np.base[target] - fb_.phys), np.stride, surf_);
    const struct ::h264ring_overlay d = descriptorFor(np, target);
    const int g = port_.show(d);
    if (g <= 0) {
        logf("overlay: show failed (" + std::to_string(g) + ")");
        return kMinRedrawMs;
    }
    ++st_.shows;
    ++st_.draws;
    if (shown_ != target)
        retireShown(uint32_t(g));
    busy_[target] = true;
    retiredValid_[target] = false;
    shown_ = target;
    shownKey_ = key;
    lastDrawMs_ = now;
    drawnDim_ = wantDim_;
    startPending(g, now, false);
    return kPollMs;
}

// OB port: the PlexUI surface. Double-buffered when two w x h pages fit fb0 (each page written
// only where it differs from what that page last held); otherwise ONE page shown once and then
// updated in place, dirty rows only: a nav step is a few rows of tearing at worst, never the
// single-page path's blank frame (720p: one page is fb0's whole smem_len, no double buffer).
inline uint64_t Renderer::writeDirtyRows(int page, const FullSurface& S, uint32_t stride) {
#ifdef MUT_OSD_SURF_ONE_SHADOW
    std::vector<uint8_t>& sh = shadow_[0];   // mutant: one shadow for both pages (diffs vs the wrong page)
    page = page & 1;
#else
    std::vector<uint8_t>& sh = shadow_[page & 1];
#endif
    const size_t row = size_t(S.w) * 4u;
    const bool full = !shadowValid_[page & 1] || sh.size() != row * S.h;
    if (full)
        sh.assign(row * S.h, 0);
    uint8_t* base = fb_.virt + (plan_.base[page & 1] - fb_.phys);
    const uint8_t* src = S.bgra.data();
    std::vector<uint8_t> dimmed;
    if (wantDim_ > 0 && S.bgra.size() >= row * S.h) {
        dimmed.assign(src, src + row * S.h);
        pauseDimApply(dimmed.data(), dimmed.size(), wantDim_);
        src = dimmed.data();
    }
    uint64_t bytes = 0;
    unsigned rows = 0;
    for (unsigned y = 0; y < S.h; ++y) {
        const uint8_t* n = src + y * row;
        uint8_t* o = sh.data() + y * row;
        size_t a = 0, b = row;
        if (!full) {
            while (a < row && std::memcmp(n + a, o + a, 4) == 0) a += 4;
            if (a == row)
                continue;
            while (b > a && std::memcmp(n + b - 4, o + b - 4, 4) == 0) b -= 4;
        }
        std::memcpy(base + size_t(y) * stride + a, n + a, b - a);
        std::memcpy(o + a, n + a, b - a);
        bytes += b - a;
        ++rows;
    }
    shadowValid_[page & 1] = true;
    st_.surfRows += rows;
    st_.surfBytes += bytes;
    return bytes;
}

inline int64_t Renderer::tickSurface(int64_t now, const FullSurface& S, const Layout& L, uint32_t acked) {
    if (!S.w || !S.h || S.w > L.W || S.h > L.H || S.bgra.size() < size_t(S.w) * S.h * 4u) {
        ++st_.surfTooBig;
        if (!surfSizeLogged_)
            logf("overlay: plexui surface " + std::to_string(S.w) + "x" + std::to_string(S.h) +
                 " does not fit the canvas " + std::to_string(L.W) + "x" + std::to_string(L.H) + "; waiting");
        surfSizeLogged_ = true;
        return shown_ >= 0 ? doHide(now) : kMinRedrawMs;
    }
    surfSizeLogged_ = false;
    if (!surfMode_) {   // from the OSD's own pages: off first, then the surface's plan
        // Same canvas as the idle card: keep the live page. A hide here left
        // Ready covering browse; a free-page show() can sit unacked (GH#27).
        if (shown_ >= 0 && (!layoutValid_ || layout_.W != S.w || layout_.H != S.h)) {
            ++st_.replans;
            return doHide(now);
        }
        surfMode_ = true;
        surfReplaceLive_ = shown_ >= 0;
        shadowValid_[0] = shadowValid_[1] = false;
        plan_ = PagePlan{};
    }
    if (shown_ >= 0 && S.gen == surfGen_ && wantDim_ == drawnDim_)
        return kMinRedrawMs;
    if (now - lastDrawMs_ < kSurfaceMinMs)
        return lastDrawMs_ + kSurfaceMinMs - now;
    const PagePlan np = planPages(fb_.phys, fb_.len, S.w, S.h, 0, S.h - 1, allowDouble_);
    if (np.buf == Buffering::None) {
        ++st_.noPage;
        if (!noPageLogged_)
            logf("overlay: no page fits fb0 (" + std::to_string(fb_.len) + ") for plexui " + std::to_string(S.w) +
                 "x" + std::to_string(S.h));
        noPageLogged_ = true;
        return shown_ >= 0 ? doHide(now) : kMinRedrawMs;
    }
    if (np != plan_) {
        if (shown_ >= 0 && (!layoutValid_ || layout_.W != S.w || layout_.H != S.h)) {
            ++st_.replans;
            return doHide(now);
        }
        logf(std::string("overlay: plexui ") + bufferingName(np.buf) + "-buffered 8888 " + std::to_string(S.w) + "x" +
             std::to_string(S.h) + (np.buf == Buffering::Single ? " (in-place dirty rows)" : "") + " in fb0 of " +
             std::to_string(fb_.len));
        plan_ = np;
        shadowValid_[0] = shadowValid_[1] = false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    auto took = [&] {
        const int64_t us =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        st_.surfLastWriteUs = us;
        st_.surfMaxWriteUs = std::max(st_.surfMaxWriteUs, us);
    };
#ifndef MUT_OSD_SURF_FLIP_IDLE
    // Ready already on this canvas: paint plexui onto the page the core is
    // scanning. A double-buffer flip to the free page is a new show() that
    // this box may never ack (restart HDMI stays Ready until nav/home).
    // surfReplaceLive_ survives kSurfaceMinMs so the first paint is not a flip.
    if (surfReplaceLive_ && shown_ >= 0) {
        writeDirtyRows(shown_, S, np.stride);
        took();
        ++st_.surfDraws;
        ++st_.surfInPlace;
        surfReplaceLive_ = false;
        surfGen_ = S.gen;
        lastDrawMs_ = now;
        drawnDim_ = wantDim_;
        if (onSurfaceShown_)
            onSurfaceShown_(S.gen);
        return kMinRedrawMs;
    }
#endif
    if (np.buf == Buffering::Single && shown_ == 0) {   // in place: the core keeps reading page 0
        writeDirtyRows(0, S, np.stride);
        took();
        ++st_.surfDraws;
        ++st_.surfInPlace;
        surfGen_ = S.gen;
        lastDrawMs_ = now;
        drawnDim_ = wantDim_;
        if (onSurfaceShown_)
            onSurfaceShown_(S.gen);
        return kMinRedrawMs;
    }
    int target = 0;
    if (np.buf == Buffering::Double) {
        target = shown_ < 0 ? 0 : 1 - shown_;
        if (!pageFree(target, acked)) {
            if (shown_ < 0 && pageFree(1 - target, acked))
                target = 1 - target;
            else
                return kPollMs;
        }
    } else if (!pageFree(0, acked)) {
        return kPollMs;
    }
    writeDirtyRows(target, S, np.stride);
    took();
    const int g = port_.show(descriptorFor(np, target));
    if (g <= 0) {
        logf("overlay: show failed (" + std::to_string(g) + ")");
        shadowValid_[target] = false;
        return kMinRedrawMs;
    }
    ++st_.shows;
    ++st_.surfDraws;
    if (shown_ != target)
        retireShown(uint32_t(g));
    busy_[target] = true;
    retiredValid_[target] = false;
    shown_ = target;
    shownKey_ = 0;
    surfGen_ = S.gen;
    surfPendingGen_ = S.gen;
    lastDrawMs_ = now;
    drawnDim_ = wantDim_;
    startPending(g, now, false);
    return kPollMs;
}

// How long OsdThread waits after a tick. GH#410: nextChange (banner 2 s) must
// cap the 8 Hz redraw so the hide is not stuck until the next content key.
inline int64_t osdSleepMs(int64_t tickMs, int64_t nextChangeAt, int64_t now, bool overlayPending) {
    if (overlayPending)
        return Renderer::kPollMs;
    int64_t d = tickMs;
    if (nextChangeAt != INT64_MAX)
        d = std::min(d, nextChangeAt - now);
    return std::max<int64_t>(1, d);
}

// Pre-tick 20 ms floor must not overshoot the banner hide (GH#410).
inline int64_t osdPreTickSleepMs(int64_t sinceLastTick, int64_t minTickMs, int64_t nextChangeAt, int64_t now,
                                 bool overlayPending) {
    if (overlayPending || sinceLastTick >= minTickMs)
        return 0;
    int64_t cap = minTickMs - sinceLastTick;
    if (nextChangeAt != INT64_MAX)
        cap = std::min(cap, nextChangeAt - now);
    return std::max<int64_t>(0, cap);
}

// The renderer's thread for one OB session (§1, §6). Not started under MPX_OB_OSD=0.
class OsdThread {
public:
    using Clock = std::function<int64_t()>;
    OsdThread(UiBus& bus, IOverlayPort& port, FbMap fb, bool allowDouble,
              std::function<void(const std::string&)> log, Clock clock)
        : bus_(bus), r_(port, fb, allowDouble, std::move(log)), clock_(std::move(clock)) {}
    ~OsdThread() { stop(); }
    OsdThread(const OsdThread&) = delete;
    OsdThread& operator=(const OsdThread&) = delete;

    void start() {
        stop_ = false;
        thr_ = std::thread([this] { loop(); });
    }
    // joins, then hides the overlay (the ring is still open)
    void stop() {
        if (!thr_.joinable())
            return;
        stop_ = true;
        bus_.wake();
        thr_.join();
        r_.shutdown([](unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); });
    }
    RendererStats stats() const {
        std::lock_guard<std::mutex> g(mu_);
        return stats_;
    }
    // OB port: before start()
    void setOnSurfaceShown(std::function<void(uint64_t)> f) { r_.setOnSurfaceShown(std::move(f)); }

private:
    bool stopping() const { return stop_.load(); }
    void loop() {
        int64_t lastTick = INT64_MIN / 2;
        while (!stopping()) {
            uint64_t gen = 0;
            UiModel m = bus_.snapshot(&gen);
            int64_t now = clock_();
            const int64_t pre = osdPreTickSleepMs(now - lastTick, kMinTickMs, m.nextChange(now), now, r_.pending());
            if (pre > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(pre));
                now = clock_();
                m = bus_.snapshot(&gen);
            }
            lastTick = now;
            if (m.spelunkrTuning && !m.idle && !m.pending && m.bannerGoneMs == 0 && !m.spelunkrDial.empty()) {
                bus_.healSpelunkrBannerHide(now);
                m = bus_.snapshot(&gen);
            }
            const UiState frame = m.resolve(now);
            int64_t d = r_.tick(now, frame);
            if (!m.serviceNotice.empty() && r_.showing(frame))
                bus_.noteServiceShown(m.serviceNotice);
            bus_.noteCanvas(r_.layout().W, r_.layout().H);
            {
                std::lock_guard<std::mutex> g(mu_);
                stats_ = r_.stats();
            }
            now = clock_();
            m = bus_.snapshot(&gen);
            const int64_t nc = m.nextChange(now);
            const int64_t sleepMs = osdSleepMs(d, nc, now, r_.pending());
            if (r_.pending())
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
            else
                bus_.waitFor(gen, sleepMs, [this] { return stop_.load(); });
        }
    }
    static constexpr int64_t kMinTickMs = 20;
    UiBus& bus_;
    Renderer r_;
    Clock clock_;
    mutable std::mutex mu_;
    std::atomic<bool> stop_{false};
    RendererStats stats_;
    std::thread thr_;
};

} // namespace misterplex::owenb_osd
