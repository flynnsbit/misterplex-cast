// N2 end-of-stream rule (inputCloseIsEndOfStream, av_inproc_decode.hpp), without libav: an EIO
// at the server's close of a length-less response is the end of the stream only near the PMS
// duration (review F1-F3, review-rel-batch-1428-1004.md section 4).
#include "libmisterplex/av_inproc_decode.hpp"

#include <cstdio>

using misterplex::inputCloseIsEndOfStream;
using misterplex::kInputCloseEndWindowMs;

static int failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

int main() {
    const int64_t dur = 120021;  // lab 4, the flash clip
    const bool unknownLength = true, knownLength = false;
    const bool live = false, cancelled = true, failed = true;
    // the window is 2 s, inclusive
    CHECK(kInputCloseEndWindowMs == 2000);
    CHECK(inputCloseIsEndOfStream(unknownLength, live, live, dur, 0, dur));
    CHECK(inputCloseIsEndOfStream(unknownLength, live, live, dur, 0, dur - 485));   // a lab MiSTer flash
    CHECK(inputCloseIsEndOfStream(unknownLength, live, live, dur, 0, dur - 2000));  // the edge
    CHECK(!inputCloseIsEndOfStream(unknownLength, live, live, dur, 0, dur - 2001)); // just past it
    CHECK(inputCloseIsEndOfStream(unknownLength, live, live, dur, 0, dur + 700));   // ran long
    // mid-stream closes stay errors
    CHECK(!inputCloseIsEndOfStream(unknownLength, live, live, dur, 0, dur / 2));
    CHECK(!inputCloseIsEndOfStream(unknownLength, live, live, dur, 0, 0));
    // F1: a known length (Content-Length) closing short keeps its EIO, even at the end
    CHECK(!inputCloseIsEndOfStream(knownLength, live, live, dur, 0, dur));
    // F3: no declared duration, a cancel, or a reader that already failed: no conversion
    CHECK(!inputCloseIsEndOfStream(unknownLength, live, live, 0, 0, dur));
    CHECK(!inputCloseIsEndOfStream(unknownLength, live, live, -1, 0, dur));
    CHECK(!inputCloseIsEndOfStream(unknownLength, cancelled, live, dur, 0, dur));
    CHECK(!inputCloseIsEndOfStream(unknownLength, live, failed, dur, 0, dur));
    // no packet read yet (span unknown)
    CHECK(!inputCloseIsEndOfStream(unknownLength, live, live, dur, 0, -1));
    // F2: session time is the play's start offset plus the span read: a resume at 60 s
    // reaches the end after a 60 s span, and the same span from 0 is mid-stream
    CHECK(inputCloseIsEndOfStream(unknownLength, live, live, dur, 60000, dur - 60000));
    CHECK(inputCloseIsEndOfStream(unknownLength, live, live, dur, 60000, dur - 60000 - 2000));
    CHECK(!inputCloseIsEndOfStream(unknownLength, live, live, dur, 60000, dur - 60000 - 2001));
    CHECK(!inputCloseIsEndOfStream(unknownLength, live, live, dur, 0, dur - 60000));
    CHECK(!inputCloseIsEndOfStream(unknownLength, live, live, dur, -5, dur));
    // the window is a parameter: 0 means only at or past the declared end
    CHECK(!inputCloseIsEndOfStream(unknownLength, live, live, dur, 0, dur - 1, 0));
    CHECK(inputCloseIsEndOfStream(unknownLength, live, live, dur, 0, dur, 0));
    if (failures) {
        std::fprintf(stderr, "test_input_close_eof: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_input_close_eof: OK\n");
    return 0;
}
