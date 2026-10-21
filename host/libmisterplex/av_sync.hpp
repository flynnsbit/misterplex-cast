#pragma once
// A/V clock-master resync for the FPGA H.264 path (avsync45, release 19:22 10/3: the 45 MHz
// core ships only if the HDMI A/V offset stays within 40 ms for the whole soak).
//
// What the daemon can see: the core freezes the audio DMA's consumed-sample count at each
// frame's actual display pickup (fpga_video_publish.sv, MVPS word 5), so every presented frame
// gives the A/V offset at the display boundary: the frame's content time minus the content time
// of the audio the hardware was consuming at that moment (+ = audio late). The FPGA path paces
// video AUs by that same consumed-sample clock, so changing which audio content the hardware
// consumes moves the offset without touching the picture (no stall, no frame drop).
//
// - PcmRateNudger: S16 stereo resampler (Catmull-Rom cubic) at r = input frames per output
//   frame. r > 1 plays the content slightly faster than the output clock (audio moves earlier
//   against the picture), r < 1 slower. r is slewed (1 % per 0.25 s at most), so neither the
//   waveform nor the pitch steps; r == 1 at phase 0 passes samples through bit-exact.
// - AudioContentMap: output frame index (what the hardware counts) -> input position (content),
//   so a frozen consumed count maps to the audio content it was playing.
// - AvSyncController: the median of the last 48 frame offsets against a reference that
//   starts at the play-start alignment (the median over 8 s after a 2 s settle) and follows
//   slow changes (120 s). Slow drift is not chased here: the daemon cannot see soak A's
//   -0.87 ms/min (it is after the display pickup), so the row's open-loop drift
//   compensation (the base ratio, avDriftPpm) cancels it. An excursion more than 40 ms from
//   the reference for 1 s starts a resync: a 1 % pull (10 ms/s) that watches the newest
//   frames and stops near the reference or once the excursion has cleared, capped; it then
//   re-measures and reports the offset before and after. A pull that did not help is undone.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

namespace misterplex {
namespace av_sync {

constexpr int kRateHz = 48000;
constexpr int kChannels = 2;
constexpr size_t kFrameBytes = 4;  // S16LE stereo

inline int64_t framesToUs(double frames) {
    return static_cast<int64_t>(std::llround(frames * 1000000.0 / kRateHz));
}

class PcmRateNudger {
public:
    static constexpr double kMinRatio = 0.98;
    static constexpr double kMaxRatio = 1.02;
    static constexpr double kMaxSlewPerFrame = 0.01 / 12000.0;  // 1 % per 0.25 s

    // Appends the output for `bytes` more input (any byte count; a partial trailing frame is
    // kept for the next call) to `out`. Returns the output frames appended. Output frame j
    // reads the input at position inputPosition() just before it; the last two input frames
    // stay buffered until more input arrives (cubic lookahead, 42 us).
    size_t process(const uint8_t* in, size_t bytes, double targetRatio, std::vector<uint8_t>& out) {
        targetRatio = std::clamp(targetRatio, kMinRatio, kMaxRatio);
        appendInput(in, bytes);
        const int64_t frames = static_cast<int64_t>(buf_.size() / kChannels);
        size_t produced = 0;
        for (;;) {
            const double local = pos_ - static_cast<double>(bufStart_);
            const double fl = std::floor(local);
            const int64_t i = static_cast<int64_t>(fl);
            if (i + 2 >= frames)
                break;
            const double t = local - fl;
            int16_t frame[kChannels];
            for (int ch = 0; ch < kChannels; ++ch) {
                const double p1 = sample(i, ch);
                if (t == 0.0) {
                    frame[ch] = static_cast<int16_t>(p1);
                    continue;
                }
                const double p0 = sample(std::max<int64_t>(i - 1, 0), ch);
                const double p2 = sample(i + 1, ch);
                const double p3 = sample(i + 2, ch);
                const double y = p1 + 0.5 * t * (p2 - p0 + t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 +
                                                                 t * (3.0 * (p1 - p2) + p3 - p0)));
                frame[ch] = static_cast<int16_t>(std::clamp(std::lround(y), -32768L, 32767L));
            }
            const size_t at = out.size();
            out.resize(at + kFrameBytes);
            std::memcpy(out.data() + at, frame, kFrameBytes);
            ++produced;
            ++outFrames_;
            ratio_ += std::clamp(targetRatio - ratio_, -kMaxSlewPerFrame, kMaxSlewPerFrame);
            pos_ += ratio_;
        }
        // Keep one frame of history before the next read position.
        const int64_t keepFrom =
            std::max<int64_t>(0, static_cast<int64_t>(std::floor(pos_)) - 1 - bufStart_);
        if (keepFrom > 0) {
            const int64_t drop = std::min<int64_t>(keepFrom, frames);
            buf_.erase(buf_.begin(), buf_.begin() + drop * kChannels);
            bufStart_ += drop;
        }
        return produced;
    }

    // Absolute input position (frames) read by the next output frame.
    double inputPosition() const { return pos_; }
    uint64_t outputFrames() const { return outFrames_; }
    double ratio() const { return ratio_; }
    // Audio content moved so far against the output clock, in microseconds (+: earlier).
    int64_t appliedUs() const { return framesToUs(pos_ - static_cast<double>(outFrames_)); }

private:
    void appendInput(const uint8_t* in, size_t bytes) {
        size_t used = 0;
        if (carryLen_ > 0) {
            while (carryLen_ < kFrameBytes && used < bytes)
                carry_[carryLen_++] = in[used++];
            if (carryLen_ < kFrameBytes)
                return;
            pushFrame(carry_);
            carryLen_ = 0;
        }
        while (bytes - used >= kFrameBytes) {
            pushFrame(in + used);
            used += kFrameBytes;
        }
        while (used < bytes)
            carry_[carryLen_++] = in[used++];
    }
    void pushFrame(const uint8_t* f) {
        int16_t s[kChannels];
        std::memcpy(s, f, kFrameBytes);
        buf_.push_back(s[0]);
        buf_.push_back(s[1]);
    }
    double sample(int64_t frame, int ch) const {
        return static_cast<double>(buf_[static_cast<size_t>(frame) * kChannels + ch]);
    }

    std::vector<int16_t> buf_;  // interleaved input frames [bufStart_, bufStart_ + size/2)
    int64_t bufStart_ = 0;
    double pos_ = 0.0;
    double ratio_ = 1.0;
    uint64_t outFrames_ = 0;
    uint8_t carry_[kFrameBytes] = {};
    size_t carryLen_ = 0;
};

class AudioContentMap {
public:
    static constexpr size_t kMaxPoints = 1024;  // about 20 s of 20 ms chunks

    // contentStartUs: content time of input frame 0 against the first video PTS.
    void reset(int64_t contentStartUs) {
        points_.clear();
        points_.push_back({0, 0.0});
        startUs_ = contentStartUs;
    }
    // Checkpoint after each nudger call: output frames so far -> input position.
    void add(uint64_t outFrame, double inPos) {
        if (!points_.empty() && outFrame <= points_.back().out)
            return;
        points_.push_back({outFrame, inPos});
        while (points_.size() > kMaxPoints)
            points_.pop_front();
    }
    // Content time of output frame n (microseconds against the first video PTS); false when n
    // is outside the kept checkpoints.
    bool contentUs(uint64_t n, int64_t& us) const {
        if (points_.empty() || n < points_.front().out || n > points_.back().out)
            return false;
        auto hi = std::lower_bound(points_.begin(), points_.end(), n,
                                   [](const Point& p, uint64_t v) { return p.out < v; });
        double in = 0.0;
        if (hi->out == n) {
            in = hi->in;
        } else {
            const auto lo = std::prev(hi);
            const double f = static_cast<double>(n - lo->out) / static_cast<double>(hi->out - lo->out);
            in = lo->in + f * (hi->in - lo->in);
        }
        us = startUs_ + framesToUs(in);
        return true;
    }

private:
    struct Point {
        uint64_t out;
        double in;
    };
    std::deque<Point> points_;
    int64_t startUs_ = 0;
};

struct AvSyncConfig {
    int64_t thresholdUs = 40000;      // an excursion: |offset - reference| over this...
    int64_t persistUs = 1000000;      // ...for this long (a passing glitch never resyncs)
    int64_t settleUs = 2000000;       // ignore offsets this long after the first one
    int64_t baselineUs = 8000000;     // then the baseline is the median over this long
    size_t window = 48;               // the measured offset is the median of this many frames
    size_t recent = 12;               // a resync tracks the median of the newest frames
    double trackTauS = 120.0;         // the reference follows slow changes (e.g. the drift
                                      // compensation's own ramp at the display boundary)
    double resyncRate = 0.01;         // pull rate (1 % = 10 ms per second)
    int64_t bandUs = 5000;            // a resync stops this close to the reference...
    int64_t capExtraUs = 10000;       // ...and never moves more than the excursion plus this
    int64_t toleranceUs = 500;
    int64_t statusEveryUs = 10000000;
};

struct AvSyncEvent {
    enum Kind { Baseline, ResyncStart, ResyncDone, UndoStart, Disabled, Status };
    Kind kind = Status;
    int64_t offsetUs = 0;     // offset vs the reference (Baseline: the baseline itself)
    int64_t beforeUs = 0;     // ResyncDone: offset vs reference when the pull started
    int64_t afterUs = 0;      // ResyncDone: offset vs reference after the re-measure
    int64_t movedUs = 0;      // ResyncDone/UndoStart: audio moved by the pull (+: earlier)
    int64_t durationUs = 0;   // ResyncDone: pull time
    int64_t referenceUs = 0;  // Status: reference minus baseline (slow change followed)
    double ppm = 0.0;         // Status: drift compensation; ResyncStart/UndoStart: pull rate
};

// Reacts only to excursions that last: the reference follows the 48-frame median with a 120 s
// time constant (slow changes, incl. the drift compensation's ramp, are followed, never
// pulled); an offset more than 40 ms from it for 1 s starts a resync. The pull watches the
// newest 12 frames and stops within 5 ms of the reference or when the excursion has cleared
// (sign change), capped at the excursion + 10 ms; one follow-up pull takes back what is left
// (a glitch that cleared during the pull leaves nothing). A pull that does not take a quarter off the
// offset is undone at the same rate and the loop turns off for the play; the drift
// compensation (the base ratio, open loop) stays.
class AvSyncController {
public:
    explicit AvSyncController(AvSyncConfig cfg = {}) : cfg_(cfg) {}

    void reset() {
        const AvSyncConfig cfg = cfg_;
        *this = AvSyncController(cfg);
    }

    // Open-loop drift compensation (row avDriftPpm): the ratio outside a pull.
    void setBaseRatio(double r) { base_ = r; }
    double baseRatio() const { return base_; }

    // One presented frame's offset at the display boundary (us, + = audio late), at nowUs.
    void addMeasurement(int64_t offsetUs, int64_t nowUs) {
        if (firstUs_ < 0)
            firstUs_ = nowUs;
        const int64_t age = nowUs - firstUs_;
        if (age < cfg_.settleUs)
            return;
        if (state_ == State::Baseline) {
            baselineSamples_.push_back(offsetUs);
            if (age >= cfg_.settleUs + cfg_.baselineUs && baselineSamples_.size() >= cfg_.window) {
                baselineUs_ = median(baselineSamples_);
                refUs_ = static_cast<double>(baselineUs_);
                baselineSamples_.clear();
                state_ = State::Watch;
                lastUs_ = lastStatusUs_ = nowUs;
                push({AvSyncEvent::Baseline, baselineUs_});
            }
            return;
        }
        push(window_, offsetUs, cfg_.window);
        push(recent_, offsetUs, cfg_.recent);
        if (window_.size() < cfg_.window)
            return;
        const int64_t med = median(window_);
        const int64_t err = med - ref();
        const int64_t now = median(recent_) - ref();
        lastErrUs_ = err;
        switch (state_) {
        case State::Watch:
            if (std::llabs(err) > cfg_.thresholdUs) {
                if (excursionSinceUs_ < 0) {
                    excursionSinceUs_ = nowUs;
                } else if (nowUs - excursionSinceUs_ >= cfg_.persistUs) {
                    startPull(std::llabs(now) > std::llabs(err) ? now : err, nowUs);
                    AvSyncEvent e{AvSyncEvent::ResyncStart, pullBeforeUs_};
                    e.ppm = pullDir_ * cfg_.resyncRate * 1e6;
                    push(e);
                }
            } else {
                excursionSinceUs_ = -1;
                const double dt = std::max(0.0, static_cast<double>(nowUs - lastUs_) / 1e6);
                refUs_ += (static_cast<double>(med) - refUs_) * std::min(1.0, dt / cfg_.trackTauS);
            }
            break;
        case State::Resync:
            if (std::llabs(now) <= cfg_.bandUs || (now > 0 ? 1 : -1) != pullDir_)
                endPull(nowUs);
            break;
        case State::Holdoff: {
            AvSyncEvent e{AvSyncEvent::ResyncDone, err};
            e.beforeUs = pullBeforeUs_;
            e.afterUs = err;
            e.movedUs = pullMovedUs_;
            e.durationUs = pullDurationUs_;
            push(e);
            excursionSinceUs_ = -1;
            const bool sameSign = (err > 0) == (pullBeforeUs_ > 0);
            if (std::llabs(err) <= cfg_.bandUs || followUp_) {
                followUp_ = false;
                state_ = State::Watch;
            } else if (sameSign && std::llabs(err) * 4 > std::llabs(pullBeforeUs_) * 3) {
                // The measurement did not follow the audio: put the audio back, then stop.
                undoPlanUs_ = -pullMovedUs_;
                undoDir_ = undoPlanUs_ > 0 ? 1 : -1;
                undoStartAppliedUs_ = appliedUs_;
                undoStartUs_ = nowUs;
                state_ = State::Undo;
                AvSyncEvent u{AvSyncEvent::UndoStart, err};
                u.movedUs = pullMovedUs_;
                u.ppm = undoDir_ * cfg_.resyncRate * 1e6;
                push(u);
            } else {
                // Partly corrected, or the excursion cleared during the pull (sign flip):
                // one follow-up pull back to the reference, so a passing glitch leaves no
                // offset behind.
                followUp_ = true;
                startPull(err, nowUs);
                AvSyncEvent f{AvSyncEvent::ResyncStart, err};
                f.ppm = pullDir_ * cfg_.resyncRate * 1e6;
                push(f);
            }
            break;
        }
        case State::Undo:
        case State::Off:
        case State::Baseline:
            break;
        }
        lastUs_ = nowUs;
        if ((state_ == State::Watch || state_ == State::Off) &&
            nowUs - lastStatusUs_ >= cfg_.statusEveryUs) {
            lastStatusUs_ = nowUs;
            AvSyncEvent e{AvSyncEvent::Status, err};
            e.referenceUs = ref() - baselineUs_;
            e.ppm = (base_ - 1.0) * 1e6;
            push(e);
        }
    }

    // The nudger's total move so far (us, + = audio earlier), after each processed chunk.
    void noteApplied(int64_t appliedUs, int64_t nowUs) {
        appliedUs_ = appliedUs;
        if (state_ == State::Resync) {
            if (std::llabs(pullMoved(nowUs)) >= pullCapUs_)
                endPull(nowUs);
        } else if (state_ == State::Undo) {
            const int64_t moved = appliedUs_ - undoStartAppliedUs_ - baseMoveUs(nowUs - undoStartUs_);
            if (undoDir_ > 0 ? moved >= undoPlanUs_ - cfg_.toleranceUs
                             : moved <= undoPlanUs_ + cfg_.toleranceUs) {
                state_ = State::Off;
                push({AvSyncEvent::Disabled, lastErrUs_});
            }
        }
    }

    // Ratio the audio pump should resample at now (input frames per output frame).
    double targetRatio() const {
        if (state_ == State::Resync)
            return base_ + pullDir_ * cfg_.resyncRate;
        if (state_ == State::Undo)
            return base_ + undoDir_ * cfg_.resyncRate;
        return base_;
    }

    bool hasBaseline() const { return state_ != State::Baseline; }
    bool resyncing() const { return state_ == State::Resync || state_ == State::Undo; }
    bool disabled() const { return state_ == State::Off; }
    int64_t baselineUs() const { return baselineUs_; }
    int64_t lastErrorUs() const { return lastErrUs_; }
    const AvSyncConfig& config() const { return cfg_; }

    std::vector<AvSyncEvent> takeEvents() {
        std::vector<AvSyncEvent> out;
        out.swap(events_);
        return out;
    }

private:
    enum class State { Baseline, Watch, Resync, Holdoff, Undo, Off };

    int64_t ref() const { return static_cast<int64_t>(std::llround(refUs_)); }
    // Content the base ratio alone moves over elapsedUs of play (us, + = earlier).
    int64_t baseMoveUs(int64_t elapsedUs) const {
        return static_cast<int64_t>(std::llround((base_ - 1.0) * static_cast<double>(elapsedUs)));
    }
    int64_t pullMoved(int64_t nowUs) const {
        return appliedUs_ - pullStartAppliedUs_ - baseMoveUs(nowUs - pullStartUs_);
    }
    void startPull(int64_t beforeUs, int64_t nowUs) {
        state_ = State::Resync;
        pullBeforeUs_ = beforeUs;
        pullDir_ = beforeUs > 0 ? 1 : -1;  // audio late: play the content faster
        pullCapUs_ = std::llabs(beforeUs) + cfg_.capExtraUs;
        pullStartAppliedUs_ = appliedUs_;
        pullStartUs_ = nowUs;
    }
    void endPull(int64_t nowUs) {
        pullMovedUs_ = pullMoved(nowUs);
        pullDurationUs_ = nowUs - pullStartUs_;
        window_.clear();  // re-measure from offsets taken after the pull
        recent_.clear();
        state_ = State::Holdoff;
    }
    static void push(std::deque<int64_t>& d, int64_t v, size_t cap) {
        d.push_back(v);
        while (d.size() > cap)
            d.pop_front();
    }
    static int64_t median(const std::deque<int64_t>& v) {
        std::vector<int64_t> s(v.begin(), v.end());
        return median(s);
    }
    static int64_t median(std::vector<int64_t> s) {
        if (s.empty())
            return 0;
        const size_t mid = s.size() / 2;
        std::nth_element(s.begin(), s.begin() + mid, s.end());
        if (s.size() % 2)
            return s[mid];
        const int64_t hi = s[mid];
        const int64_t lo = *std::max_element(s.begin(), s.begin() + mid);
        return (lo + hi) / 2;
    }
    void push(const AvSyncEvent& e) {
        if (events_.size() < 64)
            events_.push_back(e);
    }

    AvSyncConfig cfg_;
    State state_ = State::Baseline;
    double base_ = 1.0;
    int64_t firstUs_ = -1;
    std::vector<int64_t> baselineSamples_;
    std::deque<int64_t> window_;
    std::deque<int64_t> recent_;
    int64_t baselineUs_ = 0;
    double refUs_ = 0.0;
    int64_t lastErrUs_ = 0;
    int64_t lastUs_ = 0;
    int64_t lastStatusUs_ = 0;
    int64_t excursionSinceUs_ = -1;
    int64_t appliedUs_ = 0;
    int pullDir_ = 1;
    int64_t pullBeforeUs_ = 0;
    int64_t pullCapUs_ = 0;
    int64_t pullStartAppliedUs_ = 0;
    int64_t pullStartUs_ = 0;
    int64_t pullMovedUs_ = 0;
    int64_t pullDurationUs_ = 0;
    int undoDir_ = 1;
    bool followUp_ = false;
    int64_t undoPlanUs_ = 0;
    int64_t undoStartAppliedUs_ = 0;
    int64_t undoStartUs_ = 0;
    std::vector<AvSyncEvent> events_;
};

}  // namespace av_sync
}  // namespace misterplex
