// SPI ACK wait policy + status-write transaction model for misterplexd.
//
// History
// -------
// Sweep 113: bare 1e6-spin spiWord (no yield) burned ~1 core when ACK missing.
// Sweep 115: a 2 ms *wall* cap aborted status kicks → HDMI garbage (parent).
// rd-duck on 6f5bed0b: set-phase timeout returned with SSPI_STROBE still
//            asserted; enableIo(0) preserved that bit. writeStatusWordRaw
//            memcpy'd status_ before commit so partial failure poisoned shadow.
//            test_spi_txn_complete only exercised an unused helper.
//
// Contract
// --------
// 1. MUST yield after kSpiAckBusySpinsBeforeYield failed polls (CPU).
// 2. MUST NOT abort solely on a short wall clock (Sweep 115). Abort = legacy
//    poll budget kSpiAckMaxPolls only.
// 3. Aggregate *sleep* per ACK phase capped at kSpiAckMaxTotalSleepUs so
//    SpiExclusive cannot park Main under multi-second usleep; remaining polls
//    are yield-only. Healthy ACK follows clk_sys (near-immediate).
// 4. On spiWord set-phase failure after strobe assert, MUST deassert strobe
//    before return.
// 5. status_ shadow updates ONLY after a full successful multi-word write.
// 6. Production writeStatusWordRaw MUST call statusWriteWordTxnEnabled.

#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

namespace misterplex {

inline constexpr int kSpiAckBusySpinsBeforeYield = 32;
inline constexpr int kSpiAckMaxPolls = 1000000;
inline constexpr int kSpiAckYieldFloorUs = 1;
inline constexpr int kSpiAckYieldCapUs = 50;
// Cap sum of positive sleep_us over one ACK wait. After this, yield-only.
inline constexpr int kSpiAckMaxTotalSleepUs = 50000; // 50 ms

inline constexpr uint32_t kSpiStrobeBit = (1u << 17);
inline constexpr uint32_t kSpiIoEnBit = (1u << 20);
inline constexpr uint16_t kUioSetStatus2 = 0x1e;

inline constexpr int kOsdMailboxQuietMs = 200;
inline constexpr int kOsdSpiPlayingQuietMs = 500;
inline constexpr int kOsdSpiIdleQuietMs = 5000;
inline constexpr int kInputMailboxQuietMs = 100;

inline constexpr int kOsdSpiIdleQuietMsFloor = 2000;
inline constexpr int kSpiAckBusySpinsCeiling = 64;
inline constexpr bool kSpiAckUseWallTimeout = false;
inline constexpr int kSpiAckForbiddenShortWallUs = 5000;

struct SpiAckWaitStats {
    int busy_reads = 0;
    int yields = 0;
    int consecutive_busy_peak = 0;
    bool timed_out = false;
    int sleep_us_total = 0;
    int64_t wall_us = 0;
};

template <typename Pred, typename SleepUsFn>
bool spiAckWait(Pred pred, SpiAckWaitStats& st, SleepUsFn sleep_us) {
    st = SpiAckWaitStats{};
    const auto t0 = std::chrono::steady_clock::now();
    int consecutive = 0;
    int yield_us = kSpiAckYieldFloorUs;

    for (;;) {
        if (pred()) {
            st.wall_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
            return true;
        }

        ++st.busy_reads;
        ++consecutive;
        if (consecutive > st.consecutive_busy_peak)
            st.consecutive_busy_peak = consecutive;

        if (st.busy_reads >= kSpiAckMaxPolls) {
            st.timed_out = true;
            st.wall_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
            return false;
        }

        if (consecutive >= kSpiAckBusySpinsBeforeYield) {
            int use = 0;
            if (st.sleep_us_total < kSpiAckMaxTotalSleepUs) {
                use = yield_us;
                const int room = kSpiAckMaxTotalSleepUs - st.sleep_us_total;
                if (use > room)
                    use = room;
            }
            sleep_us(use);
            ++st.yields;
            if (use > 0)
                st.sleep_us_total += use;
            consecutive = 0;
            if (yield_us < kSpiAckYieldCapUs) {
                const int next = yield_us * 2;
                yield_us = next > kSpiAckYieldCapUs ? kSpiAckYieldCapUs : next;
            }
        }
    }
}

inline void spiAckDefaultSleepUs(int us) {
    if (us <= 0) {
        std::this_thread::yield();
        return;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

template <typename Pred>
bool spiAckWait(Pred pred, SpiAckWaitStats& st) {
    return spiAckWait(pred, st, spiAckDefaultSleepUs);
}

inline int osdPollQuietMs(bool via_mailbox, bool playing) {
    if (via_mailbox)
        return kOsdMailboxQuietMs;
    return playing ? kOsdSpiPlayingQuietMs : kOsdSpiIdleQuietMs;
}

// --- spiWord production model (strobe cleanup) -------------------------------
struct SpiWordTxnResult {
    uint16_t data = 0;
    bool ok = false;
    const char* err = nullptr;
    SpiAckWaitStats set_stats{};
    SpiAckWaitStats clr_stats{};
};

template <typename GpoWriteFn, typename GpiReadFn, typename SleepUsFn>
SpiWordTxnResult spiWordTxn(uint16_t word, uint32_t gpo_cur, uint32_t strobe_bit,
                            GpoWriteFn gpo_write, GpiReadFn gpi_read, SleepUsFn sleep_us) {
    SpiWordTxnResult r;
    const uint32_t gpo = (gpo_cur & ~(0xFFFFu | strobe_bit)) | static_cast<uint32_t>(word);
    gpo_write(gpo);
    gpo_write(gpo | strobe_bit);

    auto wait_phase = [&](bool want_set, SpiAckWaitStats& st, const char* timeout_msg) -> int {
        int last_gpi = 0;
        bool not_user = false;
        const bool ok = spiAckWait(
            [&]() {
                last_gpi = gpi_read();
                if (last_gpi < 0) {
                    not_user = true;
                    return true;
                }
                const bool set = (static_cast<uint32_t>(last_gpi) & strobe_bit) != 0;
                return want_set ? set : !set;
            },
            st, sleep_us);
        if (not_user) {
            r.err = "FPGA not in user mode";
            return -1;
        }
        if (!ok) {
            r.err = timeout_msg;
            return -1;
        }
        return last_gpi;
    };

    const int gpi_set = wait_phase(true, r.set_stats, "SPI ACK timeout (set)");
    if (gpi_set < 0) {
        // CRITICAL (rd-duck #1): deassert strobe before return.
        gpo_write(gpo);
        r.ok = false;
        return r;
    }

    gpo_write(gpo);
    const int gpi_clr = wait_phase(false, r.clr_stats, "SPI ACK timeout (clr)");
    if (gpi_clr < 0) {
        r.ok = false;
        return r;
    }
    r.data = static_cast<uint16_t>(gpi_clr);
    r.ok = true;
    r.err = nullptr;
    return r;
}

// --- Multi-word status write (production algorithm) --------------------------
struct SpiTxnPhaseResult {
    bool ok = false;
    const char* err = nullptr;
};

template <typename IssueFn>
bool spiTxnCommitAll(int words, IssueFn issue, int& failed_at, const char*& err) {
    failed_at = -1;
    err = nullptr;
    if (words <= 0)
        return true;
    for (int i = 0; i < words; ++i) {
        SpiTxnPhaseResult pr = issue(i);
        if (!pr.ok) {
            failed_at = i;
            err = pr.err ? pr.err : "spi phase failed";
            return false;
        }
    }
    return true;
}

inline constexpr int kStatusWriteSpiWords = 1 + 8;

// Production writeStatusWordRaw algorithm. shadow updated ONLY on full success.
template <typename SpiWordFn, typename EnableIoFn>
bool statusWriteWordTxnEnabled(const uint8_t word[16], uint8_t shadow[16], SpiWordFn spi_word,
                               EnableIoFn enable_io, int& failed_at, const char*& err) {
    failed_at = -1;
    err = nullptr;
    if (!word || !shadow) {
        err = "statusWriteWordTxn: null";
        return false;
    }

    enable_io(1);
    const bool ok = spiTxnCommitAll(
        kStatusWriteSpiWords,
        [&](int i) -> SpiTxnPhaseResult {
            if (i == 0)
                return spi_word(kUioSetStatus2);
            const int bi = (i - 1) * 2;
            const uint16_t w =
                static_cast<uint16_t>((static_cast<uint16_t>(word[bi + 1]) << 8) | word[bi]);
            return spi_word(w);
        },
        failed_at, err);
    enable_io(0);

    if (!ok)
        return false;

    std::memcpy(shadow, word, 16);
    return true;
}

inline constexpr int kSpiAckSleepUsUpperBound = kSpiAckMaxTotalSleepUs;

} // namespace misterplex
