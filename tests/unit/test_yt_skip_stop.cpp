// Gate 2b: a standalone yt-dlp timeout on a skip must leave the player idle, and
// stopVideo must stop media even when the Lounge session id is already empty.
#include "gcast/cast_glass.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using misterplex::gcast::GlassAction;
using misterplex::gcast::GlassStep;
using misterplex::gcast::Owner;
using misterplex::gcast::castGlassAction;

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                                                       \
    do {                                                                                               \
        if (c)                                                                                         \
            ++g_pass;                                                                                  \
        else {                                                                                         \
            ++g_fail;                                                                                  \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                    \
        }                                                                                              \
    } while (0)

// MediaPlayer::stop logs "media: stop" and, with the R1 idle framebuffer armed,
// paints the chevron. This fake records that pair.
struct Glass {
    bool active = false;
    bool idle = true;
    int stops = 0;

    void play() {
        active = true;
        idle = false;
    }

    void mediaStop() {
        ++stops;
        active = false;
        idle = true;
    }
};

static void apply(Glass& g, const GlassAction& act) {
    if (act.stopMedia)
        g.mediaStop();
}

static std::string slurp(const char* path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Skip while V1 is on glass. Resolve of the new id fails. The old stream is
// stopped and the chevron is up, which is the Stopped state told to the sender.
static void resolveFailOnSkip() {
    Glass g;
    g.play();
    const GlassAction begin = castGlassAction(GlassStep::NewPlay, g.active, true, Owner::YouTube,
                                              Owner::YouTube);
    CHECK(begin.stopMedia);
    CHECK(begin.idle);
    apply(g, begin);
    CHECK(g.stops == 1);
    CHECK(!g.active);
    CHECK(g.idle);

    const GlassAction failed = castGlassAction(GlassStep::ResolveFailed, g.active, true, Owner::YouTube,
                                               Owner::YouTube);
    CHECK(failed.idle);
    apply(g, failed);
    CHECK(!g.active);
    CHECK(g.idle);
    CHECK(g.stops == 1);

    // The failure path itself stops a stream that is still up (the bug's glass).
    Glass still;
    still.play();
    const GlassAction late = castGlassAction(GlassStep::ResolveFailed, still.active, true, Owner::YouTube,
                                             Owner::None);
    CHECK(late.stopMedia);
    CHECK(late.idle);
    apply(still, late);
    CHECK(still.stops == 1);
    CHECK(!still.active);
    CHECK(still.idle);
}

// stopVideo after the session id was cleared, with the old stream still active.
static void stopEmptyId() {
    Glass g;
    g.play();
    const GlassAction withId = castGlassAction(GlassStep::Stop, g.active, true, Owner::YouTube, Owner::None);
    const GlassAction emptyId = castGlassAction(GlassStep::Stop, g.active, false, Owner::YouTube, Owner::None);
    CHECK(emptyId.stopMedia == withId.stopMedia);
    CHECK(emptyId.idle == withId.idle);
    CHECK(emptyId.stopMedia);
    CHECK(emptyId.idle);
    apply(g, emptyId);
    CHECK(g.stops == 1);
    CHECK(!g.active);
    CHECK(g.idle);

    // A different owner has the player: an empty-id YouTube stop must not cut it.
    Glass plex;
    plex.play();
    const GlassAction other = castGlassAction(GlassStep::Stop, plex.active, false, Owner::YouTube, Owner::Plex);
    CHECK(!other.stopMedia);
    apply(plex, other);
    CHECK(plex.stops == 0);
    CHECK(plex.active);

    // Owning the session still stops, id or not, even if the test starts from idle.
    Glass owned;
    const GlassAction own = castGlassAction(GlassStep::Stop, owned.active, false, Owner::YouTube, Owner::YouTube);
    CHECK(own.stopMedia);
    apply(owned, own);
    CHECK(owned.stops == 1);
    CHECK(owned.idle);
}

static void libraryPlaySurvivesStrayStop() {
    Glass plex;
    plex.play();
    const GlassAction stray = castGlassAction(GlassStep::Stop, plex.active, false, Owner::YouTube,
                                              Owner::None, false);
    CHECK(!stray.stopMedia);
    CHECK(!stray.idle);
    apply(plex, stray);
    CHECK(plex.stops == 0);
    CHECK(plex.active);
}

static void seekDoesNotTearDown() {
    Glass g;
    g.play();
    const GlassAction seek = castGlassAction(GlassStep::NewPlay, g.active, true, Owner::YouTube,
                                             Owner::YouTube, true, false);
    CHECK(!seek.stopMedia);
    CHECK(!seek.idle);
    apply(g, seek);
    CHECK(g.stops == 0);
    CHECK(g.active);
    const GlassAction play = castGlassAction(GlassStep::NewPlay, g.active, true, Owner::YouTube,
                                             Owner::YouTube, true, true);
    CHECK(play.stopMedia);
}

static void daemonCallsTheSameDecision() {
    const std::string main = slurp("arm/misterplexd/main.cpp");
    CHECK(main.find("castGlassAction(GlassStep::NewPlay") != std::string::npos);
    CHECK(main.find("castGlassAction(GlassStep::ResolveFailed") != std::string::npos);
    CHECK(main.find("castGlassAction(GlassStep::Stop") != std::string::npos);
    CHECK(main.find("retirePlayback") != std::string::npos);
    CHECK(main.find("activePlayGen.load() == castGen.load()") != std::string::npos);
    CHECK(main.find("c.kind == YtCmd::Seek") != std::string::npos);
}

int main() {
    resolveFailOnSkip();
    stopEmptyId();
    libraryPlaySurvivesStrayStop();
    seekDoesNotTearDown();
    daemonCallsTheSameDecision();
    std::printf("test_yt_skip_stop: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
