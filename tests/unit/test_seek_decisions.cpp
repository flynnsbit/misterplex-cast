// Decision calls per seek burst (lead a lab MiSTer A3 1011: 4 seekTo in 2.25 s, 21:00..22:30, recovered
// in ~55 s = two serial ~24 s WAN resolves, the first superseded). A seek on the committed remote
// universal session replays it with only &offset= changed (seek_reuse.hpp); a resolve that a later
// seek supersedes never issues its decision (resolveAborted).
//   D1 cold play: exactly 1 decision.
//   D2 the A3 burst (4 seeks, 750 ms apart, each past the 500 ms settle): 0 decisions; each replay keeps
//      the session id and every transcode parameter, only offset= changes (1200..1290 s).
//   D3 cache miss (the AU-safe clamp changed the ladder, as on a prod 480p first seek from 0): the burst's
//      superseded resolves issue no decision, so the coalesced burst runs exactly 1.
//   D4 never reused: another item, server, token, core, a ladder-proxy play, a non-seek play.
// Mutants: MUT_SEEK_NO_REUSE FAILs at D2; MUT_RESOLVE_NO_ABORT FAILs at D3.
#include "seek_reuse.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace misterplex;

namespace {
int fails = 0;
void expect(bool ok, const std::string& what) {
    if (!ok) { ++fails; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}
std::string dir, callLog;
int decisions() {
    std::ifstream f(callLog);
    std::string l;
    int n = 0;
    while (std::getline(f, l)) if (l.find("/universal/decision") != std::string::npos) ++n;
    return n;
}
std::string withoutOffset(std::string u) { return universalStartWithOffset(u, 0); }
} // namespace

int main() {
    char tmpl[] = "/tmp/sdshimXXXXXX";
    dir = mkdtemp(tmpl);
    callLog = dir + "/calls";
    {
        std::ofstream f(dir + "/curl");
        f << "#!/bin/sh\necho \"$*\" >> '" << callLog << "'\nprintf '<MediaContainer generalDecisionCode=\"1000\"/>'\n";
    }
    chmod((dir + "/curl").c_str(), 0755);
    setenv("PATH", (dir + ":" + std::getenv("PATH")).c_str(), 1);

    const std::string base = "http://203.0.113.47:32401";
    WeakLadder w;
    w.videoResolution = "624x480"; w.maxVideoBitrateKbps = 2000; w.videoQuality = 40;
    w.h264Profile = "baseline"; w.h264Level = 30;
    PlayRequest play;
    play.key = "/library/metadata/11681"; play.protocol = "http"; play.address = "203.0.113.47";
    play.port = "32401"; play.token = "SERVER";
    const uint32_t core = 0;  // prod line: one core per daemon

    // D1: the cold play resolves once.
    const std::string start0 = buildUniversalTranscodeUrl(base, play.key, play.token, "mplex-abc", 0, w);
    expect(ensureUniversalDecision(start0, "mplex-abc", play.token, w), "D1 decision ok");
    expect(decisions() == 1, "D1 cold play: 1 decision, got " + std::to_string(decisions()));
    ResolveResult r0;
    r0.ok = true; r0.transcoded = true; r0.playable = start0;
    r0.httpHeaders = plexFfmpegHeaders("mplex-abc", play.token, w, true, start0);
    SeekReuseEntry entry = makeSeekReuseEntry(play, w, core, r0, base);
    expect(entry.valid, "D1 committed session is reusable");

    // D2: the A3 burst.
    const int before = decisions();
    const int64_t offs[] = {1200000, 1230000, 1260000, 1290000};
    for (int64_t off : offs) {
        PlayRequest seek = play;
        seek.seekReplay = true;
        seek.offsetMs = off;
        ResolveResult r;
        std::string b, why;
        bool reused = seekReuseFor(entry, seek, w, core, off, &r, &b, &why);
        if (!reused) {   // the full resolve (its decision) as the seek path would run it
            ensureUniversalDecision(buildUniversalTranscodeUrl(base, play.key, play.token, "mplex-new", off, w),
                                    "mplex-new", play.token, w);
            continue;
        }
        expect(r.playable.find("session=mplex-abc") != std::string::npos, "D2 same session id");
        expect(r.playable.find("&offset=" + std::to_string(off / 1000)) != std::string::npos,
               "D2 offset " + std::to_string(off / 1000) + " in " + r.playable);
        expect(withoutOffset(r.playable) == start0, "D2 only offset= differs");
        expect(r.httpHeaders == r0.httpHeaders && b == base, "D2 same FFmpeg headers and base");
        entry = makeSeekReuseEntry(seek, w, core, r, b);
    }
    expect(decisions() - before == 0, "D2 A3 burst: 0 decisions, got " + std::to_string(decisions() - before));

    // D3: cache miss (ladder changed): superseded resolves skip their decision.
    {
        WeakLadder w2 = w; w2.maxVideoBitrateKbps = 1000;
        const int b3 = decisions();
        uint64_t playGen = 0;
        for (int i = 0; i < 4; ++i) {
            PlayRequest seek = play; seek.seekReplay = true;
            ResolveResult r; std::string b, why;
            expect(!seekReuseFor(entry, seek, w2, core, offs[i], &r, &b, &why), "D3 ladder change re-resolves");
            const uint64_t gen = ++playGen;
            // Seeks 1-3: the next seek of the burst lands (playGen bumps) while this resolve is still in its
            // metadata step, i.e. before its decision; seek 4 is the one that stands.
            const uint64_t standing = i < 3 ? gen + 1 : gen;
            setResolveAbortCheck([standing, gen] { return gen != standing; });
            ensureUniversalDecision(buildUniversalTranscodeUrl(base, play.key, play.token, "mplex-n", offs[i], w2),
                                    "mplex-n", play.token, w2);
            setResolveAbortCheck({});
        }
        expect(decisions() - b3 == 1, "D3 coalesced burst (cache miss): exactly 1 decision, got " +
                                          std::to_string(decisions() - b3));
    }
    // D4
    {
        ResolveResult r; std::string b, why;
        PlayRequest s = play; s.seekReplay = true;
        PlayRequest k = s; k.key = "/library/metadata/1";
        expect(!seekReuseFor(entry, k, w, core, 5000, &r, &b, &why), "D4 another item");
        PlayRequest sv = s; sv.address = "198.51.100.153";
        expect(!seekReuseFor(entry, sv, w, core, 5000, &r, &b, &why), "D4 another server");
        PlayRequest t = s; t.token = "CONTROLLER";
        expect(!seekReuseFor(entry, t, w, core, 5000, &r, &b, &why), "D4 another token");
        expect(!seekReuseFor(entry, s, w, 0x48FE178Fu, 5000, &r, &b, &why), "D4 another core");
        PlayRequest np = play;
        expect(!seekReuseFor(entry, np, w, core, 5000, &r, &b, &why), "D4 a non-seek play");
        ResolveResult lp = r0; lp.viaLadderProxy = true;
        expect(!makeSeekReuseEntry(play, w, core, lp, base).valid, "D4 a ladder-proxy play is never reused");
        expect(universalStartWithOffset(start0 + "&offset=12", 0).find("offset=") == std::string::npos,
               "D4 offset 0 drops the parameter");
    }
    std::system(("rm -rf '" + dir + "'").c_str());
    std::printf("test_seek_decisions: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
