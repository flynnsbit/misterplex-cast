#pragma once
// Show/hide for Skip Intro, Skip Credits and the Up Next card.
// Pure: the daemon feeds readMedia() (picture time, not the bar) at about 4 Hz.
// Phase 0 (seek queued, landing not committed) hides everything.

#include "plex_markers.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace misterplex::markers {

enum class Mode : uint8_t { Off, Button, Auto };

struct NextItem {
    std::string key;
    std::string ratingKey;
    std::string playQueueItemId;
    std::string title;
    int season = -1;
    int episode = -1;
};

struct TrackerConfig {
    Mode intro = Mode::Off;
    Mode credits = Mode::Off;
    bool upNext = false;
    bool autoNext = true;
    int64_t countdownMs = 10000;
    int64_t fallbackMs = 20000;
    int64_t passoutMs = 120LL * 60 * 1000;
};

struct TrackerInput {
    int64_t mediaMs = 0;
    uint32_t stamp = 0;
    bool paused = false;
    bool buffering = false;
    bool barVisible = false;
    int64_t nowMs = 0;
    int64_t lastInputMs = -1;
};

enum class Ui : uint8_t { None, SkipIntro, SkipCredits, UpNext };
enum class Action : uint8_t { None, SeekTo, PlayNext, EndPlay };

struct TrackerOutput {
    Ui ui = Ui::None;
    int countdownS = -1;
    Action action = Action::None;
    int64_t seekMs = -1;
    int64_t markerEndMs = -1;
    const char* toast = nullptr;
};

class MarkerTracker {
public:
    static constexpr int64_t kFullShowMs = 10000;
    static constexpr int64_t kPostPlayMinMs = 5LL * 60 * 1000;

    void reset(std::vector<Marker> markers, int64_t durationMs, const TrackerConfig& cfg,
               bool preserveConsumed = false) {
        std::vector<Slot> kept;
        if (preserveConsumed)
            kept.swap(slots_);
        slots_.clear();
        for (const Marker& m : markers) {
            Slot s;
            s.marker = m;
            if (preserveConsumed) {
                for (const Slot& old : kept) {
                    if (old.marker.kind == m.kind && old.marker.startMs == m.startMs &&
                        old.marker.endMs == m.endMs) {
                        s.consumed = old.consumed;
                        s.noAuto = old.noAuto;
                        break;
                    }
                }
            }
            slots_.push_back(s);
        }
        durationMs_ = durationMs;
        cfg_ = cfg;
        finalIndex_ = -1;
        {
            std::vector<Marker> plain;
            plain.reserve(slots_.size());
            for (const Slot& s : slots_)
                plain.push_back(s.marker);
            if (const Marker* f = finalCredits(plain, durationMs_)) {
                for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
                    const Marker& mk = slots_[static_cast<std::size_t>(i)].marker;
                    if (mk.kind == f->kind && mk.startMs == f->startMs && mk.endMs == f->endMs) {
                        finalIndex_ = i;
                        break;
                    }
                }
            }
        }
        epoch_ = 0;
        haveEpoch_ = false;
        userSeekPending_ = false;
        userSeekEpoch_ = 0;
        upNextHidden_ = false;
        eofSuppressed_ = false;
        countdownElapsed_ = 0;
        countdownMark_ = -1;
        countdownFired_ = false;
        cardShowing_ = false;
        lastUi_ = Ui::None;
        visibleSinceMs_ = -1;
        buttonSlot_ = -1;
    }

    void setNext(std::optional<NextItem> n) { next_ = std::move(n); }

    bool hasNext() const { return next_.has_value(); }
    const NextItem* next() const { return next_ ? &*next_ : nullptr; }
    bool suppressNaturalEof() const { return eofSuppressed_; }
    Ui visible() const { return lastUi_; }
    int64_t visibleSinceMs() const { return visibleSinceMs_; }
    const TrackerConfig& config() const { return cfg_; }

    void noteUserSeek(uint32_t epoch) {
        if (epoch == 0)
            userSeekPending_ = true;
        else
            userSeekEpoch_ = epoch;
    }

    TrackerOutput activate(int64_t nowMs) {
        TrackerOutput o;
        o.ui = lastUi_;
        if (lastUi_ == Ui::None)
            return o;
        if (lastUi_ == Ui::UpNext) {
            o.action = Action::PlayNext;
            o.countdownS = -1;
            hideCard();
            noteVisible(Ui::None, nowMs);
            o.ui = Ui::None;
            return o;
        }
        if (buttonSlot_ >= 0 && buttonSlot_ < static_cast<int>(slots_.size())) {
            Slot& s = slots_[static_cast<std::size_t>(buttonSlot_)];
            s.consumed = true;
            o.markerEndMs = s.marker.endMs;
            if (isFinalSlot(buttonSlot_) && !hasNext())
                o.action = Action::EndPlay;
            else {
                o.action = Action::SeekTo;
                o.seekMs = s.marker.endMs + 500;
            }
            clearButtonShow(s);
            noteVisible(Ui::None, nowMs);
            o.ui = Ui::None;
        }
        return o;
    }

    void dismiss() {
        if (lastUi_ == Ui::UpNext) {
#ifndef MUT_MARKERS_UPNEXT_CANCEL_IGNORED
            hideCard();
            eofSuppressed_ = true;
            noteVisible(Ui::None, visibleSinceMs_);
#endif
            return;
        }
        if (buttonSlot_ >= 0 && buttonSlot_ < static_cast<int>(slots_.size())) {
            Slot& s = slots_[static_cast<std::size_t>(buttonSlot_)];
            s.consumed = true;
            clearButtonShow(s);
        }
        noteVisible(Ui::None, visibleSinceMs_);
    }

    TrackerOutput tick(const TrackerInput& in) {
        TrackerOutput o;
        const uint8_t phase = static_cast<uint8_t>(in.stamp & 0xffu);
        const uint32_t epoch = in.stamp >> 8;
        if (phase == 0) {
            park(in, false);
            noteVisible(Ui::None, in.nowMs);
            buttonSlot_ = -1;
            return o;
        }
        if (!haveEpoch_ || epoch != epoch_) {
            onEpoch(epoch, in.mediaMs);
            haveEpoch_ = true;
            epoch_ = epoch;
        }
        const bool frozen = in.paused || in.buffering;
        park(in, !frozen && cardShowing_);
        advanceButtons(in, !frozen);

        if (cfg_.intro == Mode::Off && cfg_.credits == Mode::Off && !cfg_.upNext) {
            noteVisible(Ui::None, in.nowMs);
            return o;
        }

        if (cardDue(in.mediaMs)) {
            o.ui = Ui::UpNext;
            o.countdownS = countdownOf(in);
            if (o.countdownS == 0 && !countdownFired_) {
                countdownFired_ = true;
                o.action = Action::PlayNext;
            }
            cardShowing_ = true;
            buttonSlot_ = -1;
            noteVisible(Ui::UpNext, in.nowMs);
            return o;
        }
        cardShowing_ = false;

        for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
            Slot& s = slots_[static_cast<std::size_t>(i)];
            if (s.marker.kind == Kind::Commercial)
                continue;
            const Mode mode = s.marker.kind == Kind::Intro ? cfg_.intro : cfg_.credits;
            if (mode == Mode::Off || s.consumed)
                continue;
            if (isFinalSlot(i) && hasNext() && cfg_.upNext)
                continue;
            const bool already = s.shownMark >= 0;
            if (!inWindow(s, in.mediaMs, already)) {
                clearButtonShow(s);
                continue;
            }
            if (mode == Mode::Auto && !s.noAuto) {
                s.consumed = true;
                clearButtonShow(s);
                o.markerEndMs = s.marker.endMs;
                o.toast = s.marker.kind == Kind::Intro ? "SKIPPED INTRO" : "SKIPPED CREDITS";
                if (isFinalSlot(i) && !hasNext())
                    o.action = Action::EndPlay;
                else {
                    o.action = Action::SeekTo;
                    o.seekMs = s.marker.endMs + 500;
                }
                noteVisible(Ui::None, in.nowMs);
                return o;
            }
            if (s.shownMark < 0)
                s.shownMark = in.nowMs;
            if (s.shownUnpausedMs >= kFullShowMs && !in.barVisible)
                continue;
            o.ui = s.marker.kind == Kind::Intro ? Ui::SkipIntro : Ui::SkipCredits;
            buttonSlot_ = i;
            noteVisible(o.ui, in.nowMs);
            return o;
        }
        buttonSlot_ = -1;
        noteVisible(Ui::None, in.nowMs);
        return o;
    }

private:
    struct Slot {
        Marker marker;
        bool consumed = false;
        bool noAuto = false;
        int64_t shownUnpausedMs = 0;
        int64_t shownMark = -1;
    };

    bool isFinalSlot(int index) const { return index >= 0 && index == finalIndex_; }

    bool inWindow(const Slot& s, int64_t media, bool already) const {
        if (media < s.marker.startMs)
            return false;
#ifndef MUT_MARKERS_INTRO_WINDOW
        if (media >= s.marker.endMs - 1000)
            return false;
        if (!already && media >= s.marker.endMs - 2000)
            return false;
#else
        (void)already;
#endif
        return true;
    }

    bool cardDue(int64_t media) const {
        if (!cfg_.upNext || upNextHidden_ || eofSuppressed_ || !next_)
            return false;
        if (durationMs_ < kPostPlayMinMs)
            return false;
        int64_t start = -1;
        if (finalIndex_ >= 0)
            start = slots_[static_cast<std::size_t>(finalIndex_)].marker.startMs;
        else if (cfg_.fallbackMs > 0)
            start = durationMs_ - cfg_.fallbackMs;
        if (start < 0 || media < start)
            return false;
        if (media >= durationMs_)
            return false;
        return true;
    }

    bool autoplay(const TrackerInput& in) const {
        if (!cfg_.autoNext)
            return false;
        if (cfg_.passoutMs > 0 && in.lastInputMs >= 0 && in.nowMs - in.lastInputMs >= cfg_.passoutMs)
            return false;
        return true;
    }

    int countdownOf(const TrackerInput& in) {
        if (!autoplay(in))
            return -1;
        if (cfg_.countdownMs <= 0)
            return 0;
        const int64_t left = cfg_.countdownMs - countdownElapsed_;
        if (left <= 0)
            return 0;
        return static_cast<int>((left + 999) / 1000);
    }

    void onEpoch(uint32_t epoch, int64_t media) {
        const bool user = userSeekPending_ || (userSeekEpoch_ != 0 && userSeekEpoch_ == epoch);
        if (userSeekPending_)
            userSeekPending_ = false;
        for (Slot& s : slots_) {
            if (media < s.marker.startMs) {
                s.consumed = false;
                s.noAuto = false;
                clearButtonShow(s);
            } else if (user && media >= s.marker.startMs && media < s.marker.endMs)
                s.noAuto = true;
        }
        int64_t cardStart = -1;
        if (finalIndex_ >= 0)
            cardStart = slots_[static_cast<std::size_t>(finalIndex_)].marker.startMs;
        else if (durationMs_ > 0 && cfg_.fallbackMs > 0)
            cardStart = durationMs_ - cfg_.fallbackMs;
        if (cardStart >= 0 && media < cardStart) {
            upNextHidden_ = false;
            eofSuppressed_ = false;
            countdownFired_ = false;
            countdownElapsed_ = 0;
            countdownMark_ = -1;
        }
    }

    void park(const TrackerInput& in, bool countCard) {
        if (countdownMark_ < 0) {
            countdownMark_ = in.nowMs;
            return;
        }
        if (countCard && in.nowMs > countdownMark_)
            countdownElapsed_ += in.nowMs - countdownMark_;
        countdownMark_ = in.nowMs;
    }

    void advanceButtons(const TrackerInput& in, bool run) {
        for (Slot& s : slots_) {
            if (s.shownMark < 0)
                continue;
            if (run && in.nowMs > s.shownMark)
                s.shownUnpausedMs += in.nowMs - s.shownMark;
            s.shownMark = in.nowMs;
        }
    }

    void clearButtonShow(Slot& s) {
        s.shownMark = -1;
        s.shownUnpausedMs = 0;
    }

    void hideCard() {
        upNextHidden_ = true;
        cardShowing_ = false;
        countdownFired_ = true;
    }

    void noteVisible(Ui ui, int64_t nowMs) {
        if (ui != Ui::None && lastUi_ == Ui::None)
            visibleSinceMs_ = nowMs;
        if (ui == Ui::None)
            visibleSinceMs_ = -1;
        lastUi_ = ui;
    }

    std::vector<Slot> slots_;
    int64_t durationMs_ = 0;
    TrackerConfig cfg_{};
    int finalIndex_ = -1;
    std::optional<NextItem> next_;
    uint32_t epoch_ = 0;
    bool haveEpoch_ = false;
    bool userSeekPending_ = false;
    uint32_t userSeekEpoch_ = 0;
    bool upNextHidden_ = false;
    bool eofSuppressed_ = false;
    int64_t countdownElapsed_ = 0;
    int64_t countdownMark_ = -1;
    bool countdownFired_ = false;
    bool cardShowing_ = false;
    Ui lastUi_ = Ui::None;
    int64_t visibleSinceMs_ = -1;
    int buttonSlot_ = -1;
};

} // namespace misterplex::markers
