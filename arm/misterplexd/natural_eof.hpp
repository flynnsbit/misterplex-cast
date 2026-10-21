#pragma once

#include "companion.hpp"
#include <atomic>
#include <cstdio>
#include <utility>

namespace misterplex {

// Current predicates run under Companion's state mutex: use only atomic reads.
template<class Current, class Advance, class Post>
void handleNaturalEof(Companion& companion, std::atomic<bool>& inFlight,
                      int64_t timeMs, int64_t durationMs, Current current,
                      Advance advance, Post post) {
    if (!current())
        return;
    if (inFlight.exchange(true)) {
        companion.setState("ended", timeMs, durationMs, true, current);
        return;
    }
    companion.setState("buffering", timeMs, durationMs, false, current);
    auto finish = [&companion, &inFlight, timeMs, durationMs, current, advance]() {
        bool advanced = false;
        try {
            if (current())
                advanced = advance();
        } catch (...) {
            std::fputs("misterplexd: auto-next exception\n", stderr);
        }
        if (!advanced)
            companion.setState("ended", timeMs, durationMs, true, current);
        inFlight.store(false);
    };
    try {
        post(std::move(finish));
    } catch (...) {
        companion.setState("ended", timeMs, durationMs, true, current);
        inFlight.store(false);
        throw;
    }
}

} // namespace misterplex
