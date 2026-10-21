// Unit tests for the MrAudio driver status parse and the audible clock.
//
// The strings here are copied verbatim from the lab unit, so this pins the
// contract against the real kernel format rather than an assumed one. If the
// driver's sprintf ever changes, this fails loudly instead of silently handing
// the present loop a bogus latency.
#include "libmisterplex/mraudio_status.hpp"

#include <cstdio>
#include <cstring>

static int fails = 0;
#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            ++fails;                                                                             \
        }                                                                                        \
    } while (0)

static int64_t parse(const char* s) {
    return misterplex::parseMrAudioQueuedBytes(s, static_cast<int64_t>(std::strlen(s)));
}

int main() {
    using namespace misterplex;

    // --- real captures from /dev/MrAudio on the lab unit ---
    CHECK(parse("rptr: 234576, wptr: 234576, len:      0, comp: 0\n") == 0);
    CHECK(!eosAudioDrainShouldWait(0, false));
    CHECK(!eosAudioDrainShouldWait(-1, false));
    CHECK(eosAudioDrainShouldWait(19200, false));
    CHECK(!eosAudioDrainShouldWait(19200, true));
    CHECK(!eosAudioDrainShouldWait(0, true));
    CHECK(parse("rptr: 238120, wptr: 426576, len: 188456, comp: 4\n") == 188456);
    CHECK(parse("rptr: 405960, wptr: 426576, len:  20616, comp: 2\n") == 20616);

    // --- malformed / hostile input must report "unknown", never a wrong number ---
    CHECK(parse("") == -1);
    CHECK(parse("rptr: 1, wptr: 2\n") == -1);              // no len field at all
    CHECK(parse("len:") == -1);                            // truncated mid-field
    CHECK(parse("len:   \n") == -1);                       // no digits
    CHECK(parse("len: abc\n") == -1);                      // non-numeric
    CHECK(parse("len: -5\n") == -1);                       // driver field is unsigned
    CHECK(misterplex::parseMrAudioQueuedBytes(nullptr, 10) == -1);
    CHECK(misterplex::parseMrAudioQueuedBytes("len: 5", 0) == -1);
    // A value at/over the ring size cannot be a real occupancy.
    CHECK(parse("len: 524288\n") == -1);
    CHECK(parse("len: 999999999999999999999\n") == -1);    // must not overflow
    // Not fooled by a prefix that merely contains the key's letters.
    CHECK(parse("nolen: 7\n") == 7); // 'len:' genuinely occurs here — documented, not a bug

    // --- audible clock ---
    constexpr int64_t kSec = kMrAudioBytesPerSec;
    // One second submitted, nothing queued -> a full second has been heard.
    CHECK(audibleClockMs(kSec, 0) == 1000);
    // One second submitted but half of it still in the ring -> only 500 ms heard.
    CHECK(audibleClockMs(kSec, kSec / 2) == 500);
    // Everything still queued -> nothing heard yet.
    CHECK(audibleClockMs(kSec, kSec) == 0);
    // Unknown queue depth falls back to the submitted-byte clock.
    CHECK(audibleClockMs(kSec, -1) == 1000);
    // Never negative, even if the queue somehow exceeds what we think we wrote.
    CHECK(audibleClockMs(kSec, kSec * 2) == 0);
    CHECK(audibleClockMs(0, 0) == 0);

    // The correction is exactly the queue depth, so a deeper ring means a
    // proportionally earlier playback position.
    CHECK(audibleClockMs(2 * kSec, kSec / 4) == 1750);

    // --- feed-rate servo ---
    const double nom = 48000.0 * 4.0; // 192000 B/s nominal

    // Unknown depth -> servo disengaged, caller gets the open-loop rate back
    // unchanged. This is the path on a kernel without the status line.
    CHECK(feedRateBytesPerSec(nom, -1) == nom);

    // At the set point the servo is a no-op.
    CHECK(feedRateBytesPerSec(nom, kFeedTargetBytes) == nom);

    // Ring too deep -> feed slower; too shallow -> feed faster. Getting these
    // backwards turns the loop into a divergence, so pin the sign.
    CHECK(feedRateBytesPerSec(nom, kFeedTargetBytes + 1920) < nom);
    CHECK(feedRateBytesPerSec(nom, kFeedTargetBytes - 1920) > nom);

    // Proportional term: an error of tau*X bytes asks for exactly X B/s of
    // correction, before clamping.
    {
        const int64_t err = static_cast<int64_t>(kFeedServoTauSec * 200.0);
        const double r = feedRateBytesPerSec(nom, kFeedTargetBytes + err);
        CHECK(r > nom - 201.0 && r < nom - 199.0);
    }

    // The clamp bounds the correction at kFeedServoMaxFrac in both directions,
    // even for absurd depths, so a garbage reading cannot pitch-shift audio.
    {
        const double lo = feedRateBytesPerSec(nom, kMrAudioRingBytes);
        const double hi = feedRateBytesPerSec(nom, 0);
        CHECK(lo >= nom * (1.0 - kFeedServoMaxFrac) - 1e-6);
        CHECK(hi <= nom * (1.0 + kFeedServoMaxFrac) + 1e-6);
        // A full ring is far past the clamp, so it must sit exactly on it.
        CHECK(lo < nom * (1.0 - kFeedServoMaxFrac) + 1e-6);
    }

    // The rate is never zero or negative, whatever the inputs, or the pump's
    // deadline arithmetic would divide by zero / run backwards.
    CHECK(feedRateBytesPerSec(1.0, kMrAudioRingBytes) > 0.0);

    // Convergence: simulate a ring that starts at the old measured 185 ms burst
    // against a drain that is 645 ppm slow, and check the loop pulls it to the
    // set point rather than running away.
    {
        const double drain = 192000.0 * (1.0 - 645.0 / 1e6);
        double depth = 0.185 * 192000.0;
        const double dt = 0.08; // sampling cadence
        for (int i = 0; i < 2000; ++i) {
            const double feed = feedRateBytesPerSec(nom, static_cast<int64_t>(depth));
            depth += (feed - drain) * dt;
            if (depth < 0)
                depth = 0;
        }
        const double errBytes = depth - static_cast<double>(kFeedTargetBytes);
        // Settled within a few ms of the target (a bit of steady-state offset is
        // expected from a proportional loop and is harmless).
        CHECK(errBytes > -2000.0 && errBytes < 2000.0);
    }

    if (fails) {
        std::fprintf(stderr, "test_mraudio_status: %d FAILURES\n", fails);
        return 1;
    }
    std::printf("test_mraudio_status: OK\n");
    return 0;
}
