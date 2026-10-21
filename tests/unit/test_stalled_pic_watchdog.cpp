#include "libmisterplex/stalled_pic_watchdog.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

int fails = 0;
void expect(bool c, const char* m) {
    if (!c) { std::fprintf(stderr, "FAIL stalled pic: %s\n", m); ++fails; }
}
std::string slurp(const char* p) {
    std::ifstream f(p);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
int main() {
    using misterplex::stalledPicShouldReopen;
    using misterplex::stalledPicShouldFail;
    expect(!stalledPicShouldReopen(false, 0, 3), "idle must not reopen");
    expect(!stalledPicShouldFail(false, 0, 3), "idle must not fail");
    expect(stalledPicShouldReopen(true, 0, 3), "hits 0 must reopen");
    expect(stalledPicShouldReopen(true, 1, 3), "hits 1 must reopen");
    expect(stalledPicShouldReopen(true, 2, 3), "hits 2 must reopen");
    expect(!stalledPicShouldReopen(true, 3, 3), "hits 3 must not reopen");
    expect(stalledPicShouldFail(true, 3, 3), "hits 3 must fail");
    expect(!stalledPicShouldFail(true, 2, 3), "hits 2 must not fail");
    const std::string cpp = slurp("arm/misterplexd/media_player.cpp");
    const std::string hpp = slurp("arm/misterplexd/media_player.hpp");
    expect(hpp.find("stalledPicWatchdogHits_") != std::string::npos,
           "hpp must declare stalledPicWatchdogHits_");
    expect(hpp.find("mutable int range_retry_") == std::string::npos,
           "range_retry_ must not be mutable");
    expect(cpp.find("#if defined(MPX_STALLED_PIC_WATCHDOG)") == std::string::npos,
           "live path must not be ifdef-off");
    expect(cpp.find("boundedReopen") != std::string::npos,
           "cpp must call boundedReopen (shared hang reopen, 375+378)");
    const auto i = cpp.find("if (isPicStalled())");
    expect(i != std::string::npos, "cpp must test isPicStalled");
    if (i != std::string::npos) {
        const std::string chunk = cpp.substr(i, 1600);
        expect(chunk.find("boundedReopen") != std::string::npos,
               "stall site must boundedReopen at current position");
        expect(chunk.find("shouldBoundedReopen") != std::string::npos,
               "stall site must use shouldBoundedReopen");
        expect(chunk.find("failFpgaSession(\"picture delivery stalled (watchdog)\")")
                   != std::string::npos,
               "exhausted retries must still failFpgaSession");
        expect(chunk.find("goto retry_demux") == std::string::npos,
               "stall must not goto retry_demux (audioThr_ terminate)");
    }
    const auto h = cpp.find("http_range demux error");
    expect(h != std::string::npos, "cpp must log http_range demux error");
    if (h != std::string::npos) {
        const std::string chunk = cpp.substr(h, 900);
        expect(chunk.find("boundedReopen") != std::string::npos,
               "http_range demux error must boundedReopen (049 N1 dead session)");
        expect(chunk.find("shouldBoundedReopen") != std::string::npos,
               "http_range demux error must use shouldBoundedReopen");
        expect(chunk.find("http_range stall") != std::string::npos,
               "http_range demux error must log http_range stall");
        expect(chunk.find("pos <= startMs") != std::string::npos,
               "same-URL retry only before pictures (049 N1)");
        expect(chunk.find("lastGateRefusal") != std::string::npos ||
                   cpp.find("!demux.lastGateRefusal().valid") != std::string::npos,
               "GATE refusal must not take the http_range retry path");
    }
    expect(cpp.find("range_retry_ > 0 && !urlHasUniversalOffset(url)") != std::string::npos,
           "live universal must not av_seek on range_retry (049 N2)");
    const auto o = cpp.find("http_range open-error — boundedReopen");
    expect(o != std::string::npos, "open-error after range_retry must boundedReopen");
    if (o != std::string::npos) {
        const std::string chunk = cpp.substr(o > 500 ? o - 500 : 0, 500);
        expect(chunk.find("range_retry_ > 0") != std::string::npos,
               "open-error reopen only after a prior range retry");
        expect(chunk.find("shouldBoundedReopen") != std::string::npos,
               "open-error must use shouldBoundedReopen");
    }
    const auto p = cpp.find("void MediaPlayer::fpgaThreadMain");
    expect(p != std::string::npos, "fpgaThreadMain present");
    if (p != std::string::npos) {
        const std::string head = cpp.substr(p, 2400);
        expect(head.find("range_retry_ = 0") != std::string::npos,
               "each FPGA session must reset range_retry_ (049 N2 stale seek)");
    }
    const auto play = cpp.find("bool MediaPlayer::play(const std::string& urlOrPath, int64_t startOffsetMs");
    expect(play != std::string::npos, "play() present");
    if (play != std::string::npos) {
        const std::string head = cpp.substr(play, 900);
        expect(head.find("range_retry_ = 0") != std::string::npos,
               "play() must reset range_retry_ so N1 does not poison N2");
    }
    const auto q = cpp.find("bool MediaPlayer::isPicStalled()");
    const auto r = cpp.find("PlaybackSummary MediaPlayer::lastPlaybackSummary");
    expect(q != std::string::npos && r > q, "isPicStalled body present");
    if (q != std::string::npos && r > q) {
        const std::string body = cpp.substr(q, r - q);
        expect(body.find("stalledPicWatchdogHits_") == std::string::npos,
               "isPicStalled must not mutate stalledPicWatchdogHits_");
    }
    {   // PAUSE-WATCHDOG: replay of a lab MiSTer hold-3 timeline (1 tick = 1 us, 5000 ms timeout).
        using misterplex::stalledPicGapExceeded;
        const int64_t ms = 1000;
        const int64_t lastPic = 22600 * ms;            // last picture just before pause
        const int64_t resume = lastPic + 8046 * ms;    // 8 s paused, then resume()
        expect(!stalledPicGapExceeded(true, true, lastPic + 9000 * ms, lastPic, 0, ms, 5000),
               "paused 9 s must not be a stall");
        expect(!stalledPicGapExceeded(true, false, resume + 1, lastPic, resume, ms, 5000),
               "right after a long pause + resume must not be a stall (the b89bc987 bug)");
        expect(!stalledPicGapExceeded(true, false, resume + 4999 * ms, lastPic, resume, ms, 5000),
               "4.999 s after resume without a picture is not yet a stall");
        expect(stalledPicGapExceeded(true, false, resume + 5000 * ms, lastPic, resume, ms, 5000),
               "5 s after resume without a picture is a stall");
        expect(stalledPicGapExceeded(true, false, lastPic + 5000 * ms, lastPic, 0, ms, 5000),
               "no pause: 5 s without a picture is a stall (watchdog unchanged)");
        expect(!stalledPicGapExceeded(true, false, lastPic + 4999 * ms, lastPic, 0, ms, 5000),
               "no pause: 4.999 s is not a stall");
        expect(stalledPicGapExceeded(true, false, lastPic + 6000 * ms, lastPic, lastPic - 9000 * ms, ms, 5000),
               "an old resume before the last picture does not extend the window");
        expect(!stalledPicGapExceeded(false, false, lastPic + 60000 * ms, lastPic, 0, ms, 5000),
               "before the first picture there is no stall");
        if (q != std::string::npos && r > q)
            expect(cpp.substr(q, r - q).find("stalledPicGapExceeded") != std::string::npos,
                   "isPicStalled must use stalledPicGapExceeded");
        const auto rs = cpp.find("void MediaPlayer::resume()");
        expect(rs != std::string::npos && cpp.substr(rs, 2600).find("resumeArmTicks_.store") != std::string::npos,
               "resume() must stamp resumeArmTicks_");
    }
    std::printf("test_stalled_pic_watchdog: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
