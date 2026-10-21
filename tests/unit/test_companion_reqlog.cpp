// Companion request latency log rows (review 03:24, release 03:25):
// arm/misterplexd/companion_reqlog.hpp, the helper that the single companion HTTP thread calls
// once per request. A slow handler or a slow header read prints WARN at >= 1000 ms. Fast poll
// paths fold into one line per 10 s, but a slow poll is never folded. The query never reaches
// the log. Mutants (Makefile test_companion_reqlog.mutant-check) must FAIL:
//   MUT_COMPANION_SLOW_GT             threshold '>' instead of '>=' (the 1000 ms row)
//   MUT_COMPANION_POLL_SWALLOWS_SLOW  the poll window hides a slow poll (the 8 s poll row)
#include "companion_reqlog.hpp"

#include <cstdio>
#include <string>

using namespace misterplex;

static int fails = 0;
static void expect(bool ok, const char* what, const std::string& got) {
    if (!ok) {
        std::printf("FAIL %s\n  got: %s\n", what, got.c_str());
        ++fails;
    }
}
static std::string str(const std::optional<std::string>& l) { return l ? *l : "(no line)"; }
static bool has(const std::optional<std::string>& l, const char* s) {
    return l && l->find(s) != std::string::npos;
}

int main() {
    const int64_t wall = 1759650000123;  // a fixed wall time; only the formatting is checked
    {
        CompanionReqLog log;
        auto l = log.observe("POST /player/playback/playMedia", wall, 3, 40, 0);
        expect(has(l, "companion: req recv=") && has(l, "path=POST /player/playback/playMedia") &&
                   has(l, "hdr_ms=3 handler_ms=40") && !has(l, "WARN"),
               "fast control request: one plain line with path, hdr_ms, handler_ms", str(l));
        l = log.observe("POST /player/playback/playMedia", wall, 2, 1200, 10);
        expect(has(l, "companion: WARN slow req") && has(l, "handler_ms=1200"),
               "slow handler (1200 ms) prints WARN", str(l));
        l = log.observe("GET /player/playback/seekTo", wall, 0, 999, 20);
        expect(l && !has(l, "WARN"), "999 ms handler: no WARN", str(l));
        l = log.observe("GET /player/playback/seekTo", wall, 0, 1000, 30);
        expect(has(l, "WARN"), "1000 ms handler: WARN (>=)", str(l));
        l = log.observe("(header timeout)", wall, 5000, 1, 40);
        expect(has(l, "WARN") && has(l, "path=(header timeout)") && has(l, "hdr_ms=5000"),
               "5 s header read (stalled client) prints WARN", str(l));
    }
    {
        CompanionReqLog log;
        auto l = log.observe("GET /player/timeline/poll", wall, 0, 400, 0);
        expect(has(l, "polls=1") && !has(l, "WARN"), "first poll prints the summary line", str(l));
        int folded = 0;
        for (int t = 1; t <= 9; ++t)
            folded += !log.observe(t % 3 ? "GET /player/timeline/poll" : "GET /player/timeline/mirror",
                                   wall, 0, 400, t * 1000);
        expect(folded == 9, "fast polls and mirrors inside the 10 s window fold (no line)",
               std::to_string(folded) + " of 9 folded");
        l = log.observe("GET /player/timeline/poll", wall, 0, 8000, 9500);
        expect(has(l, "WARN") && has(l, "handler_ms=8000"),
               "8 s poll inside the window still prints WARN at once", str(l));
        l = log.observe("GET /resources", wall, 0, 5, 10500);
        expect(has(l, "polls=10") && has(l, "max_handler_ms=400"),
               "next window: summary counts the 9 folded polls + this one (10) and their max; the WARN poll is not folded", str(l));
    }
    {
        const std::string req =
            "GET /player/timeline/poll?wait=1&commandID=7&X-Plex-Token=SECRETTOKEN HTTP/1.1\r\n"
            "Host: x\r\nX-Plex-Token: SECRETTOKEN\r\n\r\n";
        const std::string p = companionRequestPath(req);
        expect(p == "GET /player/timeline/poll", "path drops the query", p);
        expect(p.find("SECRET") == std::string::npos, "no token in the path", p);
        const std::string odd = companionRequestPath("GET /a\x01" "b HTTP/1.1\r\n\r\n");
        expect(odd == "GET /a?b", "control bytes become '?'", odd);
        const std::string longp = companionRequestPath("GET /" + std::string(200, 'x') + " HTTP/1.1\r\n");
        expect(longp.size() == kCompanionPathMax + 3 && longp.substr(longp.size() - 3) == "...",
               "path cut at 96 chars + '...'", longp);
        expect(companionRequestPath("garbage") == "(no request line)", "no request line", "");
    }
    if (fails) {
        std::printf("test_companion_reqlog: %d failures\n", fails);
        return 1;
    }
    std::printf("test_companion_reqlog: 14 rows OK\n");
    return 0;
}
