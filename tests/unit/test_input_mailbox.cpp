#include "libmisterplex/input_mailbox.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            ++fails;                                                                             \
        }                                                                                        \
    } while (0)

static uint64_t word(uint16_t seq, uint8_t cmdSeq, misterplex::PlaybackCommand cmd,
                     uint32_t magic = misterplex::kInputMailboxMagic) {
    return static_cast<uint64_t>(magic) |
           (static_cast<uint64_t>(static_cast<uint8_t>(cmd)) << 32) |
           (static_cast<uint64_t>(cmdSeq) << 40) | (static_cast<uint64_t>(seq) << 48);
}

static uint32_t lo(uint64_t v) { return static_cast<uint32_t>(v); }
static uint32_t hi(uint64_t v) { return static_cast<uint32_t>(v >> 32); }

static uint64_t frameWord(uint8_t seq, uint8_t debug, uint16_t underrun,
                          uint32_t magic = misterplex::kUnderrunMailboxMagic) {
    return static_cast<uint64_t>(magic) | (static_cast<uint64_t>(seq) << 32) |
           (static_cast<uint64_t>(debug) << 40) | (static_cast<uint64_t>(underrun) << 48);
}

struct FakeTransport {
    bool playing = true;
    bool paused = false;
    int64_t pos = 0;
    int64_t dur = 120000;
    int pauses = 0;
    int resumes = 0;
    int stops = 0;
    std::vector<int64_t> seeks;

    misterplex::PlaybackTransportState state() const { return {playing, paused, pos, dur}; }

    int calls() const {
        return pauses + resumes + stops + static_cast<int>(seeks.size());
    }

    void pause() {
        ++pauses;
        paused = true;
    }

    void resume() {
        ++resumes;
        paused = false;
    }

    void stop() {
        ++stops;
        playing = false;
    }

    void seekMs(int64_t ms) {
        seeks.push_back(ms);
        pos = ms;
    }
};

static bool pollWord(misterplex::InputMailboxEdgeDetector& edge, FakeTransport& transport,
                     uint64_t firstRead, uint64_t secondRead, int64_t nowMs = 1000,
                     int64_t ignoreUntilMs = 0) {
    using namespace misterplex;
    InputMailboxSample sample;
    if (!decodeStableInputMailbox(lo(firstRead), hi(firstRead), lo(secondRead), hi(secondRead),
                                  sample)) {
        if (lo(firstRead) == lo(secondRead) && hi(firstRead) == hi(secondRead) &&
            lo(firstRead) != kInputMailboxMagic) {
            edge.noteNoValidWord();
        }
        return false;
    }
    PlaybackCommand cmd = PlaybackCommand::None;
    if (!edge.accept(sample, cmd))
        return false;
    const int before = transport.calls();
    (void)dispatchPlaybackCommand(cmd, transport.state(), 30000, 10000, nowMs, ignoreUntilMs,
                                  transport);
    return transport.calls() != before;
}

int main() {
    using namespace misterplex;

    InputMailboxSample s;
    CHECK(decodeInputMailboxWord(word(1, 1, PlaybackCommand::None), s));
    CHECK(s.command == PlaybackCommand::None);
    CHECK(decodeInputMailboxWord(word(2, 2, PlaybackCommand::PlayPause), s));
    CHECK(s.command == PlaybackCommand::PlayPause);
    CHECK(decodeInputMailboxWord(word(3, 3, PlaybackCommand::Stop), s));
    CHECK(s.command == PlaybackCommand::Stop);
    CHECK(decodeInputMailboxWord(word(7, 9, PlaybackCommand::SkipForward), s));
    CHECK(s.seq == 7);
    CHECK(s.cmdSeq == 9);
    CHECK(s.command == PlaybackCommand::SkipForward);
    CHECK(decodeInputMailboxWord(word(8, 10, PlaybackCommand::SkipBack), s));
    CHECK(s.command == PlaybackCommand::SkipBack);
    CHECK(!decodeInputMailboxWord(word(7, 9, PlaybackCommand::SkipForward, 0xDEADBEEFu), s));
    CHECK(!decodeInputMailboxWord(static_cast<uint64_t>(kInputMailboxMagic) |
                                      (static_cast<uint64_t>(0x7Fu) << 32),
                                  s));

    FrameStoreStatus fs;
    CHECK(decodeFrameStoreStatusWord(frameWord(4, kFrameStoreDebugFormatError, 17), fs));
    CHECK(fs.seq == 4);
    CHECK(fs.debug_state == kFrameStoreDebugFormatError);
    CHECK(fs.underrun_count == 17);
    CHECK(fs.nonYuvDoorbellRejected());
    CHECK(std::string(frameStoreDebugDescription(fs.debug_state)) ==
          "frame store refused non-YUV doorbell (0xE1)");
    CHECK(std::string(frameStoreStatusUnavailableDescription()) ==
          "frame store status unavailable (PLXF mailbox absent/unwritten)");
    CHECK(!decodeFrameStoreStatusWord(frameWord(4, kFrameStoreDebugFormatError, 17, 0), fs));
    CHECK(decodeStableFrameStoreStatus(lo(frameWord(5, 0x23, 7)),
                                       hi(frameWord(5, 0x23, 7)),
                                       lo(frameWord(5, 0x23, 7)),
                                       hi(frameWord(5, 0x23, 7)), fs));
    CHECK(fs.seq == 5 && fs.debug_state == 0x23 && fs.underrun_count == 7);
    CHECK(!decodeStableFrameStoreStatus(lo(frameWord(5, 0x23, 7)),
                                        hi(frameWord(5, 0x23, 7)),
                                        lo(frameWord(6, 0x23, 7)),
                                        hi(frameWord(6, 0x23, 7)), fs));

    const uint64_t good = word(10, 1, PlaybackCommand::PlayPause);
    CHECK(decodeStableInputMailbox(lo(good), hi(good), lo(good), hi(good), s));
    CHECK(!decodeStableInputMailbox(lo(good), hi(good), lo(good) ^ 1u, hi(good), s));
    CHECK(!decodeStableInputMailbox(lo(good), hi(good), lo(good), hi(good) ^ 0x10000u, s));

    InputMailboxEdgeDetector edge;
    PlaybackCommand cmd = PlaybackCommand::None;
    CHECK(!edge.accept({1, 1, PlaybackCommand::PlayPause}, cmd)); // first valid word is stale
    CHECK(cmd == PlaybackCommand::None);
    CHECK(!edge.accept({1, 1, PlaybackCommand::PlayPause}, cmd)); // non-zero cmd alone is not enough
    CHECK(edge.accept({2, 2, PlaybackCommand::PlayPause}, cmd));
    CHECK(cmd == PlaybackCommand::PlayPause);
    CHECK(edge.accept({3, 3, PlaybackCommand::PlayPause}, cmd)); // repeated same command
    CHECK(cmd == PlaybackCommand::PlayPause);
    CHECK(!edge.accept({3, 4, PlaybackCommand::Stop}, cmd)); // seq must advance too
    CHECK(!edge.accept({4, 4, PlaybackCommand::None}, cmd));
    CHECK(edge.accept({5, 255, PlaybackCommand::SkipForward}, cmd));
    CHECK(cmd == PlaybackCommand::SkipForward);
    CHECK(edge.accept({6, 0, PlaybackCommand::SkipBack}, cmd)); // cmd_seq wrap 255->0
    CHECK(cmd == PlaybackCommand::SkipBack);
    edge.reset();
    edge.noteNoValidWord();
    CHECK(edge.accept({1, 1, PlaybackCommand::Stop}, cmd)); // no stale magic before first command
    CHECK(cmd == PlaybackCommand::Stop);

    {
        InputMailboxEdgeDetector ed;
        FakeTransport tr;
        CHECK(!pollWord(ed, tr, word(1, 1, PlaybackCommand::SkipForward, 0), // bad magic
                        word(1, 1, PlaybackCommand::SkipForward, 0)));
        CHECK(tr.calls() == 0);
        CHECK(pollWord(ed, tr, word(1, 1, PlaybackCommand::SkipForward),
                       word(1, 1, PlaybackCommand::SkipForward)));
        CHECK(tr.seeks.size() == 1 && tr.seeks.back() == 30000);
        for (int i = 0; i < 5; ++i) {
            CHECK(!pollWord(ed, tr, word(1, 1, PlaybackCommand::SkipForward),
                            word(1, 1, PlaybackCommand::SkipForward)));
        }
        CHECK(tr.seeks.size() == 1); // stale repeated mailbox word fires zero more times
        CHECK(pollWord(ed, tr, word(2, 2, PlaybackCommand::SkipForward),
                       word(2, 2, PlaybackCommand::SkipForward)));
        CHECK(pollWord(ed, tr, word(3, 3, PlaybackCommand::SkipForward),
                       word(3, 3, PlaybackCommand::SkipForward)));
        CHECK(tr.seeks.size() == 3); // identical cmd, new cmd_seq -> one dispatch each
        CHECK(tr.seeks[0] == 30000 && tr.seeks[1] == 60000 && tr.seeks[2] == 90000);
    }

    {
        InputMailboxEdgeDetector ed;
        FakeTransport tr;
        const uint64_t oldHalf = word(10, 10, PlaybackCommand::Stop);
        const uint64_t newHalf = word(11, 11, PlaybackCommand::Stop);
        CHECK(!pollWord(ed, tr, oldHalf, newHalf)); // torn seq/cmd_seq read
        CHECK(tr.calls() == 0);
        CHECK(!pollWord(ed, tr, word(12, 12, PlaybackCommand::PlayPause),
                        word(12, 12, PlaybackCommand::Stop))); // inconsistent command half
        CHECK(tr.calls() == 0);
    }

    {
        InputMailboxEdgeDetector ed;
        FakeTransport tr;
        CHECK(!pollWord(ed, tr, word(1, 1, PlaybackCommand::Stop, 0x12345678u),
                        word(1, 1, PlaybackCommand::Stop, 0x12345678u)));
        CHECK(tr.calls() == 0);
    }

    {
        InputMailboxEdgeDetector ed;
        FakeTransport tr;
        ed.noteNoValidWord();
        tr.pos = 10000;
        CHECK(pollWord(ed, tr, word(20, 255, PlaybackCommand::SkipBack),
                       word(20, 255, PlaybackCommand::SkipBack)));
        tr.pos = 50000;
        CHECK(pollWord(ed, tr, word(21, 0, PlaybackCommand::SkipBack),
                       word(21, 0, PlaybackCommand::SkipBack)));
        CHECK(tr.seeks.size() == 2);
        CHECK(tr.seeks[0] == 0);
        CHECK(tr.seeks[1] == 40000); // 255->0 is one normal event, not 255 events
    }

    auto a = mapPlaybackCommand(PlaybackCommand::PlayPause, true, false, 1000, 10000, 30000,
                                10000);
    CHECK(a.kind == PlaybackActionKind::Pause);
    a = mapPlaybackCommand(PlaybackCommand::PlayPause, true, true, 1000, 10000, 30000, 10000);
    CHECK(a.kind == PlaybackActionKind::Resume);
    a = mapPlaybackCommand(PlaybackCommand::Stop, true, false, 1000, 10000, 30000, 10000);
    CHECK(a.kind == PlaybackActionKind::Stop);
    a = mapPlaybackCommand(PlaybackCommand::SkipForward, true, false, 1000, 45000, 30000,
                            10000);
    CHECK(a.kind == PlaybackActionKind::Seek);
    CHECK(a.seekTargetMs == 31000);
    a = mapPlaybackCommand(PlaybackCommand::SkipForward, true, false, 40000, 45000, 30000,
                            10000);
    CHECK(a.kind == PlaybackActionKind::Seek);
    CHECK(a.seekTargetMs == 45000);
    a = mapPlaybackCommand(PlaybackCommand::SkipBack, true, false, 5000, 45000, 30000, 10000);
    CHECK(a.kind == PlaybackActionKind::Seek);
    CHECK(a.seekTargetMs == 0);
    a = mapPlaybackCommand(PlaybackCommand::SkipBack, true, false, 0, 45000, 30000, 10000);
    CHECK(a.kind == PlaybackActionKind::None);
    a = mapPlaybackCommand(PlaybackCommand::SkipForward, false, false, 1000, 45000, 30000,
                            10000);
    CHECK(a.kind == PlaybackActionKind::None);

    {
        FakeTransport tr;
        (void)dispatchPlaybackCommand(PlaybackCommand::PlayPause, tr.state(), 30000, 10000, 1000,
                                      0, tr);
        CHECK(tr.paused);
        CHECK(tr.pauses == 1);
        (void)dispatchPlaybackCommand(PlaybackCommand::PlayPause, tr.state(), 30000, 10000, 1000,
                                      0, tr);
        CHECK(!tr.paused);
        CHECK(tr.resumes == 1);
    }

    {
        FakeTransport tr;
        tr.pos = 5000;
        (void)dispatchPlaybackCommand(PlaybackCommand::SkipBack, tr.state(), 30000, 10000, 1000,
                                      0, tr);
        CHECK(tr.seeks.size() == 1 && tr.seeks.back() == 0);
        tr.pos = 119000;
        tr.dur = 120000;
        (void)dispatchPlaybackCommand(PlaybackCommand::SkipForward, tr.state(), 30000, 10000,
                                      1000, 0, tr);
        CHECK(tr.seeks.size() == 2 && tr.seeks.back() == 120000);
    }

    {
        FakeTransport tr;
        (void)dispatchPlaybackCommand(PlaybackCommand::Stop, tr.state(), 30000, 10000, 1000,
                                      1500, tr);
        CHECK(tr.calls() == 0);
        CHECK(tr.playing);
        (void)dispatchPlaybackCommand(PlaybackCommand::Stop, tr.state(), 30000, 10000, 1500,
                                      1500, tr);
        CHECK(tr.stops == 1);
        CHECK(!tr.playing);
    }

    // --- PLXD bank-release mailbox tests ---
    {
        BankReleaseStatus br;

        // Helper: build a PLXD word matching w-a3's RTL layout (b187df5).
        // [31:0]=magic, [33:32]=free_bank_mask, [34]=disp_bank,
        // [35]=swap_pending, [63:48]=frames_done
        auto plxd = [](uint8_t fbm, uint8_t db, bool sp, uint16_t fd) -> uint64_t {
            return static_cast<uint64_t>(kBankReleaseMailboxMagic) |
                   (static_cast<uint64_t>(fbm & 3u) << 32) |
                   (static_cast<uint64_t>(db & 1u) << 34) |
                   (static_cast<uint64_t>(sp ? 1u : 0u) << 35) |
                   (static_cast<uint64_t>(fd) << 48);
        };

        // Bank 0 free, disp_bank=1, no swap pending, frames_done=42
        const uint64_t w1 = plxd(0x01, 1, false, 42);
        CHECK(decodeBankReleaseWord(w1, br));
        CHECK(br.free_bank_mask == 0x01);
        CHECK(br.bank0Free());
        CHECK(!br.bank1Free());
        CHECK(br.anyFree());
        CHECK(br.freeBank() == 0);
        CHECK(br.disp_bank == 1);
        CHECK(!br.swap_pending);
        CHECK(br.frames_done == 42);

        // Bank 1 free, disp_bank=0, swap pending, frames_done=1000
        const uint64_t w2 = plxd(0x02, 0, true, 1000);
        CHECK(decodeBankReleaseWord(w2, br));
        CHECK(br.free_bank_mask == 0x02);
        CHECK(!br.bank0Free());
        CHECK(br.bank1Free());
        CHECK(br.freeBank() == 1);
        CHECK(br.disp_bank == 0);
        CHECK(br.swap_pending);
        CHECK(br.frames_done == 1000);

        // Both banks free (idle — no pending frame)
        const uint64_t w3 = plxd(0x03, 0, false, 500);
        CHECK(decodeBankReleaseWord(w3, br));
        CHECK(br.free_bank_mask == 0x03);
        CHECK(br.bank0Free() && br.bank1Free());
        CHECK(br.freeBank() == 0); // picks lowest

        // No bank free (both in use — swap pending)
        const uint64_t w4 = plxd(0x00, 1, true, 99);
        CHECK(decodeBankReleaseWord(w4, br));
        CHECK(br.free_bank_mask == 0x00);
        CHECK(!br.anyFree());
        CHECK(br.freeBank() == -1);
        CHECK(br.swap_pending);

        // Bad magic
        CHECK(!decodeBankReleaseWord(0xDEADBEEFu, br));

        // Stable decode
        CHECK(decodeStableBankRelease(lo(w1), hi(w1), lo(w1), hi(w1), br));
        CHECK(br.bank0Free() && br.disp_bank == 1);

        // Torn read
        CHECK(!decodeStableBankRelease(lo(w1), hi(w1), lo(w2), hi(w2), br));
    }

    if (fails) {
        std::fprintf(stderr, "test_input_mailbox: %d failure(s)\n", fails);
        return 1;
    }
    std::printf("test_input_mailbox: OK\n");
    return 0;
}
