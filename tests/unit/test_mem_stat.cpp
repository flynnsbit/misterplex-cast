// P6-MEMORY: MPX_MEMSTAT_SEC parsing and MEMSTAT line format (lab-only heap telemetry).
#include "mem_stat.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace misterplex::mem_stat;
static int pass = 0, fail = 0;
#define CHECK(c) do { if (c) ++pass; else { ++fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
int main() {
    CHECK(parsePeriodSec(nullptr) == 0);
    CHECK(parsePeriodSec("") == 0);
    CHECK(parsePeriodSec("0") == 0);
    CHECK(parsePeriodSec("-5") == 0);
    CHECK(parsePeriodSec("5s") == 0);
    CHECK(parsePeriodSec("abc") == 0);
    CHECK(parsePeriodSec("1") == 1);
    CHECK(parsePeriodSec("5") == 5);
    CHECK(parsePeriodSec("3600") == 3600);
    CHECK(parsePeriodSec("3601") == 0);           // MUT_MEMSTAT_NO_UPPER
    CHECK(parsePeriodSec("99999999999") == 0);    // MUT_MEMSTAT_NO_UPPER
    Sample s; s.inuseBytes = 2048 * 1024; s.mmapBytes = 1024 * 1024; s.freeBytes = 512 * 1024;
    s.arenaBytes = 4096 * 1024; s.topPad = 100 * 1024;
    const std::string line = format(s, 7);
    CHECK(line == "MEMSTAT seq=7 live_kb=3072 inuse_kb=2048 mmap_kb=1024 free_kb=512 arena_kb=4096 keep_kb=100"
                   " vmrss_kb=0 vmhwm_kb=0 anon_kb=0 memavail_kb=0 out_q=0 ready_q=0 probe_q=0 ytdlp_kb=0");
    const char* status =
        "Name:\tmisterplexd\n"
        "VmRSS:\t   3456 kB\n"
        "VmHWM:\t   7890 kB\n"
        "RssAnon:\t   2222 kB\n";
    const char* meminfo = "MemTotal:       524288 kB\nMemAvailable:   131072 kB\n";
    const char* rollup = "Anonymous:     3333 kB\nRss:           3456 kB\n";
    Sample parsed;
    parseProcStatus(status, parsed);
    parseMeminfo(meminfo, parsed);
    parseSmapsRollup(rollup, parsed);
    CHECK(parsed.vmRssKb == 3456 && parsed.vmHwmKb == 7890 && parsed.anonKb == 2222);
    CHECK(parsed.memAvailableKb == 131072);
    Sample noAnon;
    parseSmapsRollup(rollup, noAnon);
    CHECK(noAnon.anonKb == 3333);
    setQueueProbe([] { QueueDepths q; q.out = 4; q.ready = 5; q.probe = 64; return q; });
    noteYtdlpPeakKb(1024);
    const Sample live = sampleNow();
    CHECK(live.outQ == 4 && live.readyQ == 5 && live.probeQ == 64 && live.ytdlpKb == 1024);
    CHECK(live.vmRssKb > 0 && live.memAvailableKb > 0);
    setQueueProbe({});
    // Huge values must not truncate the key fields the soak parser greps for.
    Sample big; big.inuseBytes = big.mmapBytes = big.freeBytes = big.arenaBytes = big.topPad = (1ULL << 40);
    const std::string bl = format(big, 123456789);
    CHECK(bl.find("keep_kb=1073741824") != std::string::npos);
    // Unset env: logger does not start.
    ::unsetenv("MPX_MEMSTAT_SEC");
    CHECK(startFromEnv() == 0);
    ::setenv("MPX_MEMSTAT_SEC", "junk", 1);
    CHECK(startFromEnv() == 0);
#if defined(__GLIBC__)
    // Live sample reflects a held allocation (glibc host).
    const Sample a = sampleNow();
    std::vector<char>* v = new std::vector<char>(256 * 1024, 1);
    const Sample b = sampleNow();
    CHECK(b.inuseBytes + b.mmapBytes >= a.inuseBytes + a.mmapBytes + 200 * 1024);
    delete v;
#endif
    std::printf("mem_stat: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
