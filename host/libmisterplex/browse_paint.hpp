#pragma once

#include "browse_render.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace misterplex {
namespace browse {

enum class IdleFbPath {
    WriteCombining,
    DevMem,
};

inline const char* idleFbPathName(IdleFbPath path) {
    return path == IdleFbPath::WriteCombining ? "wc" : "devmem";
}

struct BrowsePaintAck {
    bool complete = false;
    bool timedOut = false;
    bool idleShown = false;
    int idleBufShown = -1;
    // The request can never latch: a newer PLXH request (any writer: a session's en=0, a repoint)
    // has latched instead (single slot, last write wins). There is no late latch to guard.
    bool superseded = false;
};

class BrowsePaintTarget {
public:
    virtual ~BrowsePaintTarget() = default;
    virtual IdleFbPath path() const = 0;
    virtual BrowsePaintAck pollAck(std::uint32_t token, std::uint32_t timeoutMs) = 0;
    virtual void writeLine(int idleBuf, int y, const std::uint8_t* rgb24Line) = 0;
    // One PLXH request for idleBuf. The token comes from the ONE sequence owned by the PLXH link (w9 r1_plxh.hpp
    // PlxhSessionPolicy::requestIdle: 16-bit, never 0); 0 = refused (e.g. between session BEGIN and END).
    virtual std::uint32_t requestFlip(int idleBuf) = 0;
    // Non-blocking. Asked on every pump() with no paint or repoint in flight, even with nothing pending, so the
    // target's pacer runs from the ACK rather than from the next submit. False while no paint may start: between
    // session BEGIN and END (review 19:16 (1)), and on /dev/mem until F5 pacing after the previous paint ACK is done
    // (review 19:27: 2 same-buffer no-op PLXH latches; w9 PlxhF5Pacer::ready()).
    virtual bool flipAllowed() { return true; }
    // Called once per paint ACK with the idle buffer that ACK made shown (F5 pacing starts here). Not for repoints.
    virtual void paintAcked(int /*shownBuf*/) {}
};

enum class PaintStep {
    Idle,
    Started,
    Acked,
    AckTimeout,
    RepointStarted,
    RepointAcked,
    NoChange,
    Refused, // flipAllowed() false or requestFlip() refused: nothing shown, retried later
    Superseded, // the awaited request can never latch (BrowsePaintAck::superseded): resolved
};

struct PaintResult {
    PaintStep step = PaintStep::Idle;
    int targetBuf = -1;
    int linesCopied = 0;
    int bytesCopied = 0;
    std::uint32_t token = 0;
};

class BrowsePaintPlanner {
public:
    BrowsePaintPlanner()
        : shadow_(kCanvasH * kStrideRgb24),
          previousShadow_(kCanvasH * kStrideRgb24),
          dirtyNow_{},
          dirtyPrevious_{} {}

    const char* pathLogLine(IdleFbPath path) const {
        return path == IdleFbPath::WriteCombining ? "r1: idlefb path=wc" : "r1: idlefb path=devmem";
    }

    void arm() {
        fullNext_ = true;
        fullPending_[0] = true;
        fullPending_[1] = true;
        inFlight_ = false;
        rePointInFlight_ = false;
        rePointNeeded_ = false;
        pending_ = false;
        dirtyPrevious_.fill(false);
        target_ = 0;
        inFlightBuf_ = -1;
    }

    void submit(const RenderModel& model) {
        pendingModel_ = model;
        pendingFrame_.clear();
        pending_ = true;
    }

    // A whole 640x480 RGB24 canvas (render spec S0 IDLE: renderIdleRgb24), painted through the same
    // dirty-line, double-buffer and late-latch logic as a browse model. A wrong size is ignored.
    void submitFrame(std::vector<std::uint8_t> rgb24) {
        if (rgb24.size() != shadow_.size())
            return;
        pendingFrame_ = std::move(rgb24);
        pending_ = true;
    }

    // Anything left to do: a pending model, a paint or repoint ACK, or a repoint.
    bool busy() const { return pending_ || inFlight_ || rePointInFlight_ || rePointNeeded_; }

    bool inFlight() const { return inFlight_; }
    std::uint32_t inFlightToken() const { return inFlightToken_; }

    PaintResult pump(BrowsePaintTarget& target) {
        PaintResult result{};
        if (inFlight_) {
            BrowsePaintAck ack = target.pollAck(inFlightToken_, 50);
            if (ack.superseded) {
                // Never shown and never will be: both buffers are repainted in full.
                inFlight_ = false;
                fullNext_ = true;
                fullPending_[0] = true;
                fullPending_[1] = true;
                pending_ = true;
                result.step = PaintStep::Superseded;
                result.token = inFlightToken_;
                return result;
            }
            if (ack.timedOut) {
                inFlight_ = false;
#ifndef MUT_NO_FULL_AFTER_SKIP
                fullNext_ = true;
                fullPending_[0] = true;
                fullPending_[1] = true;
#endif
#ifndef MUT_NO_REPOINT_AFTER_TIMEOUT
                rePointNeeded_ = true;
                staleToken_ = inFlightToken_;
#endif
                pending_ = true;
                result.step = PaintStep::AckTimeout;
                result.token = inFlightToken_;
                return result;
            }
            if (!ack.complete)
                return result;
            inFlight_ = false;
            target.paintAcked(inFlightBuf_);
            result.step = PaintStep::Acked;
            result.token = inFlightToken_;
            return result;
        }

        if (rePointInFlight_) {
            BrowsePaintAck ack = target.pollAck(rePointToken_, 50);
            if (ack.superseded) {
                // A newer request latched after the repoint: the stale paint token is dead too.
                rePointInFlight_ = false;
                rePointNeeded_ = false;
                staleToken_ = 0;
                result.step = PaintStep::Superseded;
                result.token = rePointToken_;
                return result;
            }
            if (ack.timedOut) {
                rePointInFlight_ = false;
                rePointNeeded_ = true;
                result.step = PaintStep::AckTimeout;
                result.token = rePointToken_;
                return result;
            }
            if (!ack.complete)
                return result;
            rePointInFlight_ = false;
            rePointNeeded_ = false;
            result.step = PaintStep::RepointAcked;
            result.token = rePointToken_;
            return result;
        }

        if (rePointNeeded_) {
            BrowsePaintAck shown = target.pollAck(0, 0);
            if (shown.idleShown && (shown.idleBufShown == 0 || shown.idleBufShown == 1)) {
                // Bug late-latch guard: a PLXH request selecting the already
                // shown idle buffer is a display no-op, but still latches its
                // fresh token into PLXO. That supersedes any stale timed-out
                // request before we write either buffer again.
                rePointToken_ = target.requestFlip(shown.idleBufShown);
                if (rePointToken_ == 0) {
                    result.step = PaintStep::Refused;
                    return result;
                }
                rePointInFlight_ = true;
                result.step = PaintStep::RepointStarted;
                result.targetBuf = shown.idleBufShown;
                result.token = rePointToken_;
                return result;
            }
            if (staleToken_ != 0) {
                BrowsePaintAck stale = target.pollAck(staleToken_, 50);
                if (stale.superseded) {
                    const std::uint32_t deadToken = staleToken_;
                    rePointNeeded_ = false;
                    staleToken_ = 0;
                    result.step = PaintStep::Superseded;
                    result.token = deadToken;
                    return result;
                }
                if (stale.complete) {
                    const std::uint32_t latchedToken = staleToken_;
                    rePointNeeded_ = false;
                    staleToken_ = 0;
                    result.step = PaintStep::RepointAcked;
                    result.token = latchedToken;
                    return result;
                }
                if (stale.timedOut) {
                    result.step = PaintStep::AckTimeout;
                    result.token = staleToken_;
                    return result;
                }
            }
            return result;
        }

        const bool allowed = target.flipAllowed();
        if (!pending_)
            return result;
        if (!allowed) {
            result.step = PaintStep::Refused; // in a session, or F5 pacing not done: the pending model waits
            return result;
        }

        pending_ = false;
        if (!pendingFrame_.empty()) {
            std::memcpy(shadow_.data(), pendingFrame_.data(), shadow_.size());
        } else {
            const RenderModel model = pendingModel_;
            for (int y = 0; y < kCanvasH; ++y)
                renderBrowseLine(model, y, shadow_.data() + static_cast<std::size_t>(y) * kStrideRgb24);
        }

        int dirtyCount = 0;
        for (int y = 0; y < kCanvasH; ++y) {
            const std::size_t off = static_cast<std::size_t>(y) * kStrideRgb24;
            dirtyNow_[static_cast<std::size_t>(y)] =
                std::memcmp(shadow_.data() + off, previousShadow_.data() + off, kStrideRgb24) != 0;
            if (dirtyNow_[static_cast<std::size_t>(y)])
                ++dirtyCount;
        }

        if (!fullNext_ && dirtyCount == 0) {
            result.step = PaintStep::NoChange;
            return result;
        }

        BrowsePaintAck shown = target.pollAck(0, 0);
        int chosen = target_;
        if (shown.idleShown && shown.idleBufShown == chosen)
            chosen = 1 - chosen;

        std::array<bool, kCanvasH> copySet{};
        if (fullNext_) {
            copySet.fill(true);
        } else {
            for (int y = 0; y < kCanvasH; ++y) {
#ifdef MUT_COPY_DN_ONLY
                copySet[static_cast<std::size_t>(y)] = dirtyNow_[static_cast<std::size_t>(y)];
#else
                copySet[static_cast<std::size_t>(y)] =
                    dirtyNow_[static_cast<std::size_t>(y)] || dirtyPrevious_[static_cast<std::size_t>(y)];
#endif
            }
        }

        if (fullNext_) {
            if (shown.idleShown && fullPending_[chosen] && shown.idleBufShown == chosen)
                chosen = 1 - chosen;
            if (!fullPending_[chosen] && fullPending_[1 - chosen] &&
                !(shown.idleShown && shown.idleBufShown == 1 - chosen)) {
                chosen = 1 - chosen;
            }
        }

        const int firstBuf = fullNext_ ? 0 : chosen;
        const int lastBuf = fullNext_ ? 1 : chosen;
        int lines = 0;
        for (int buf = firstBuf; buf <= lastBuf; ++buf) {
            if (fullNext_) {
                if (!fullPending_[buf])
                    continue;
                if (shown.idleShown && shown.idleBufShown == buf)
                    continue;
            }
            for (int y = 0; y < kCanvasH; ++y) {
                if (!copySet[static_cast<std::size_t>(y)])
                    continue;
                target.writeLine(buf, y, shadow_.data() + static_cast<std::size_t>(y) * kStrideRgb24);
                ++lines;
            }
            if (fullNext_)
                fullPending_[buf] = false;
        }
        if (lines == 0) {
            pending_ = true;
            result.step = PaintStep::Idle;
            return result;
        }

        const std::uint32_t tok = target.requestFlip(chosen);
        if (tok == 0) {
            // Refused after the copy (a session began meanwhile): nothing was flipped, so the next paint copies both
            // buffers in full again.
            fullNext_ = true;
            fullPending_[0] = true;
            fullPending_[1] = true;
            pending_ = true;
            result.step = PaintStep::Refused;
            return result;
        }
        std::memcpy(previousShadow_.data(), shadow_.data(), shadow_.size());
        dirtyPrevious_ = dirtyNow_;
        fullNext_ = fullPending_[0] || fullPending_[1];
        if (fullNext_)
            pending_ = true;
        inFlightToken_ = tok;
        inFlightBuf_ = chosen;
        inFlight_ = true;
        target_ = 1 - chosen;
        result.step = PaintStep::Started;
        result.targetBuf = chosen;
        result.linesCopied = lines;
        result.bytesCopied = lines * kStrideRgb24;
        result.token = inFlightToken_;
        return result;
    }

private:
    std::vector<std::uint8_t> shadow_;
    std::vector<std::uint8_t> previousShadow_;
    std::array<bool, kCanvasH> dirtyNow_;
    std::array<bool, kCanvasH> dirtyPrevious_;
    RenderModel pendingModel_{};
    std::vector<std::uint8_t> pendingFrame_; // non-empty: the pending item is a whole canvas
    bool pending_ = false;
    bool fullNext_ = true;
    bool fullPending_[2] = {true, true};
    bool inFlight_ = false;
    bool rePointNeeded_ = false;
    bool rePointInFlight_ = false;
    int target_ = 0;
    int inFlightBuf_ = -1;
    std::uint32_t inFlightToken_ = 0;
    std::uint32_t staleToken_ = 0;
    std::uint32_t rePointToken_ = 0;
};

} // namespace browse
} // namespace misterplex
