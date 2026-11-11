#pragma once
// P3: prefetchSpelunkr's result -> the play loop's skip-before-fail decision, pulled out as a
// pure function so the ordering (a dead-air timeout tries the next item first; any other
// prefetch failure does not) is unit-testable without the play loop's threads/locks/player.

namespace misterplex {
namespace spelunkr {

// True: attempt takeSkip(skipDead(np), "prefetch timeout") before any fail(), mirroring the
// existing "unsupported resolution" skip path. A dead-air prefetch timeout (the 15 s cap) must
// not hard-fail the channel when a next item exists; any other prefetch failure (format,
// network, caps) is not a "dead item" and fails today's way, unchanged.
inline bool shouldSkipOnPrefetchTimeout(bool prefetchTimedOut) {
    return prefetchTimedOut;
}

// The skip-attempt cap: a channel whose items are all dead/timing out must terminate instead
// of looping forever. skipsAfterIncrement is the caller's skip counter already incremented for
// this attempt (main.cpp's ++skips); itemCount is the channel's item count. A 0- or 1-item
// channel allows no skip at all (every attempt is already exhausted): std::max(1, itemCount)
// keeps the cap at least 1 so the comparison below is always well-formed.
inline bool skipBudgetExhausted(int skipsAfterIncrement, int itemCount) {
    const int cap = itemCount > 1 ? itemCount : 1;
    return skipsAfterIncrement >= cap;
}

}  // namespace spelunkr
}  // namespace misterplex
