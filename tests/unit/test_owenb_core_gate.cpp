// OWENB-P1B T3: core gate on a PlexOB core (owenb's H.264 decoder, fork named PlexOB).
// Design: OWENB-P1B-CAST-DESIGN.md §c.2, §e.3 T3. Host only: core_gate.hpp, owenb_core_identity.hpp and
// video_backend.hpp on a fake control block and a sparse /dev/mem file. No FPGA, no MiSTer.
//   L  the identity constants against the vendored h264ring_layout.h / h264ring.h.
//   N  names: the CONF_STR rename hook (PlexOB) is accepted, owenb's stock "H264" is refused, and a
//      name mismatch holds. Legacy Plex holds are unchanged.
//   I  ring identity: magic, version, geometry, an advancing heartbeat, the MPX BUILD_ID word (0x58,
//      tag 'MPXB'), the 0x0B id family, and lab-only unstamped builds.
//   G  gate: an OB open keeps open() false (FpgaSpi's legacy traffic stays closed) with owenbOpen()
//      true. Retries, refusals, a reload closing it.
//   T  coreGateTickStep: ringProbe on PlexOB, CAPS probe otherwise. No coreReturned or repaint on OB.
//   S  backend selection: sessionVideoBackend / configuredVideoBackend.
//   M  OwenbCtrlPageMap / owenbGateRingProbe on a sparse file: reads the ctrl page, PROT_READ, never
//      touches 0x31600000.
//   P  source: paintIdle returns on ownsIdleInFabric() before gateRepaintOnce_ is read.
//      --mutate-paint-order runs the same check on a reordered copy, which must FAIL.
#include "libmisterplex/core_gate.hpp"
#include "libmisterplex/owenb_core_identity.hpp"
#include "libmisterplex/video_backend.hpp"
#include "h264ring.h"
#include "h264ring_layout.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MPX_SOURCE_ROOT
#define MPX_SOURCE_ROOT "."
#endif

using namespace misterplex;

static int fails = 0, checks = 0;
#define CHECK(cond, what)                                                                    \
    do {                                                                                     \
        ++checks;                                                                            \
        if (!(cond)) {                                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, what, #cond); \
            ++fails;                                                                         \
        }                                                                                    \
    } while (0)

// L: the identity header is self-contained; these tie it to the vendored layout.
static_assert(owenb_id::kMagic == H264RING_MAGIC, "magic");
static_assert(owenb_id::kVersion == H264RING_VERSION, "version");
static_assert(owenb_id::kCtrlPhys == H264RING_CTRL_BYTE, "ctrl");
static_assert(owenb_id::kRingBytes == H264RING_RING_SIZE, "ring size");
static_assert(owenb_id::kWordGeometry == H264RING_RING_SIZE_W && H264RING_RING_SIZE_LSB == 32, "geometry");
static_assert(owenb_id::kMpxBuildIdWord == H264RING_STATUS_WORDS, "BUILD_ID word: first past CORE3");
static_assert(owenb_id::kMpxBuildIdWord * 8 < owenb_id::kCtrlBytes, "inside the ctrl page");
static_assert(owenb_id::kIdleFbPhys >= owenb_id::kCtrlPhys + owenb_id::kCtrlBytes &&
                  owenb_id::kIdleFbPhys < H264RING_RING_BYTE,
              "idle FB lies between the ctrl page and the ring");

// A fake PlexOB control block on a fake clock. alive: the heartbeat advances by one per 16 ms.
struct FakeCtrl {
    uint64_t w[512] = {};
    bool alive = true;
    int64_t now = 1000000;
    int64_t lastBeat = 0;
    void plexob(bool stamped, uint32_t id = 0x0B000201u) {
        std::memset(w, 0, sizeof w);
        w[0] = H264RING_MAGIC;
        w[1] = H264RING_VERSION;
        w[2] = static_cast<uint64_t>(H264RING_RING_SIZE) << 32;
        w[3] = 7;
        if (stamped)
            w[owenb_id::kMpxBuildIdWord] = (static_cast<uint64_t>(owenb_id::kMpxBuildIdTag) << 32) | id;
    }
    OwenbCtrlReader reader() {
        OwenbCtrlReader r;
        r.read = [this](unsigned i) { return i < 512 ? w[i] : 0; };
        r.sleepUs = [this](unsigned us) {
            now += us;
            if (alive && now - lastBeat >= 16000) {
                lastBeat = now;
                w[3] = (w[3] & ~0xFFFFFFFFull) | static_cast<uint32_t>(w[3] + 1);
            }
        };
        r.nowUs = [this] { return now; };
        return r;
    }
};

// A gate on a live fabric with Main up. Names are written after Main started.
struct Rig {
    CoreGate g;
    CoreObservation o;
    int64_t now = 10000000;
    int64_t mt = 2000;
    Rig() {
        g.resetForTest(false);
        o.fabric.known = true;
        o.fabric.mode = kFpgaMgrModeUser;
        o.main.pid = 100;
        o.main.startTicks = 1;
        o.mainStartWallNs = 1000;
        o.bootWallNs = 0;
    }
    void load(const std::string& core, const std::string& rbf) {
        o.coreName = {true, core, ++mt};
        o.rbfName = {true, rbf, ++mt};
    }
    CoreGateEvent tick(int64_t adv = 700000) {
        now += adv;
        o.nowUs = now;
        return g.observe(o);
    }
    // Ticks until ProbeWanted (true) or n ticks pass (false).
    bool toProbe(int n = 40) {
        for (int i = 0; i < n; ++i)
            if (tick() == CoreGateEvent::ProbeWanted)
                return true;
        return false;
    }
    std::string logs() {
        std::string s;
        for (const auto& l : g.takeLogs())
            s += l + "\n";
        return s;
    }
};

static bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

static void testNames() {
    CHECK(classifyLoadedCore("PlexOB", "_Utility/PlexOB_20261016.rbf") == CoreFamily::PlexOwenb, "N1 PlexOB pair");
    CHECK(classifyLoadedCore("PLEXOB", "plexob.rbf") == CoreFamily::PlexOwenb, "N1 upper/lower case");
    CHECK(classifyLoadedCore("H264", "_Utility/H264.rbf") == CoreFamily::RawH264, "N1 stock H264");
    CHECK(classifyLoadedCore("h264", "PlexOB.rbf") == CoreFamily::RawH264, "N1 stock name with an OB rbf");
    CHECK(classifyLoadedCore("PlexOB", "Plex_480p.rbf") == CoreFamily::Mismatch, "N1 OB name, legacy rbf");
    CHECK(classifyLoadedCore("Plex", "PlexOB.rbf") == CoreFamily::Mismatch, "N1 legacy name, OB rbf");
    CHECK(classifyLoadedCore("Plex", "_Utility/Plex_20260101.rbf") == CoreFamily::PlexLegacy, "N1 legacy");
    CHECK(classifyLoadedCore("MENU", "menu.rbf") == CoreFamily::NotPlex, "N1 menu");
    CHECK(classifyLoadedCore("", "x.rbf") == CoreFamily::Unknown, "N1 empty");
    CHECK(std::string(kOwenbCoreName) == "PlexOB" && std::string(kOwenbUpstreamCoreName) == "H264",
          "N1 the CONF_STR hook names");

    struct Row { const char* core; const char* rbf; bool probes; const char* hold; };
    const Row rows[] = {
        {"H264", "_Utility/H264.rbf", false, "is owenb's stock H.264 core"},          // G2: mutant opens it
        {"H264", "_Utility/PlexOB.rbf", false, "is owenb's stock H.264 core"},
        {"PlexOB", "_Utility/H264.rbf", false, "is not a PlexOB core"},
        {"PlexOB", "_Utility/Plex_20260101.rbf", false, "is not a PlexOB core"},
        {"Plex", "_Utility/PlexOB_20261016.rbf", false, "is a PlexOB core but CORENAME is 'Plex'"},
        {"MENU", "menu.rbf", false, "loaded core 'MENU' is not Plex"},
        {"Plex", "NES.rbf", false, "RBFNAME 'NES.rbf' is not a Plex core"},
        {"PlexOB", "_Utility/PlexOB_20261016.rbf", true, nullptr},
        {"Plex", "_Utility/Plex_20260101.rbf", true, nullptr},
    };
    for (const Row& r : rows) {
        Rig rig;
        rig.load(r.core, r.rbf);
        const bool probes = rig.toProbe(6);
        const std::string l = rig.logs();
        std::string what = std::string("N2 ") + r.core + " + " + r.rbf;
        CHECK(probes == r.probes, what.c_str());
        if (r.hold)
            CHECK(has(l, r.hold), (what + " hold text").c_str());
    }
}

static void testIdentity() {
    FakeCtrl f;
    OwenbIdentity id = readOwenbIdentity(f.reader(), 100);
    CHECK(id.verdict == OwenbIdVerdict::NoMagic, "I1 zero page: no magic");
    CHECK(readOwenbIdentity(OwenbCtrlReader{}, 100).verdict == OwenbIdVerdict::NoMap, "I1 no reader: NoMap");

    f.plexob(false);
    f.w[1] = 0x201;
    CHECK(readOwenbIdentity(f.reader(), 100).verdict == OwenbIdVerdict::BadVersion, "I2 major 2");
    f.w[1] = 0x100;
    CHECK(readOwenbIdentity(f.reader(), 100).verdict == OwenbIdVerdict::BadVersion, "I2 older minor");
    f.w[1] = 0x102;
    CHECK(readOwenbIdentity(f.reader(), 100).verdict == OwenbIdVerdict::Alive, "I2 newer minor");
    f.plexob(false);
    f.w[2] = static_cast<uint64_t>(0x400000u) << 32;
    CHECK(readOwenbIdentity(f.reader(), 100).verdict == OwenbIdVerdict::BadGeometry, "I3 ring size");

    // I4: a frozen heartbeat is a previous load's words (mutant MUT_OWENB_ID_SKIP_HEARTBEAT).
    f.plexob(false);
    f.alive = false;
    const int64_t t0 = f.now;
    id = readOwenbIdentity(f.reader(), 100);
    CHECK(id.verdict == OwenbIdVerdict::Stale, "I4 frozen heartbeat: Stale");
    CHECK(f.now - t0 >= 100000 && f.now - t0 < 110000, "I4 watched for heartbeatMs");
    f.alive = true;
    id = readOwenbIdentity(f.reader(), 100);
    CHECK(id.verdict == OwenbIdVerdict::Alive && id.heartbeat1 != id.heartbeat0, "I4 advancing: Alive");

    // I5: unstamped (no MPX BUILD_ID word yet) -> 0x0B000000 | version, lab-only.
    CHECK(!id.stamped && id.buildId == 0x0B000101u, "I5 unstamped id");
    CHECK(has(owenbIdentityRefusal(id, false), "lab-only"), "I5 unstamped refused by default");
    CHECK(owenbIdentityRefusal(id, true).empty(), "I5 unstamped allowed in the lab");

    // I5b: T10's stamped id opens with the lab flag unset. Unstamped 0x0B000101 stays closed.
    f.plexob(true, 0x0B593F92u);
    id = readOwenbIdentity(f.reader(), 100);
    CHECK(id.stamped && id.buildId == 0x0B593F92u, "I5b T10 id");
    CHECK(owenbIdentityRefusal(id, false).empty(), "I5b T10 accepted without unstamped");
    const OwenbBuildInfo* t10 = recognisedOwenbBuild(0x0B593F92u);
    CHECK(t10 && !t10->labOnly && std::string(t10->label) == "PlexOB T10 MPXB stamped", "I5b T10 row");
    f.plexob(true, 0x0B59C11Au);
    id = readOwenbIdentity(f.reader(), 100);
    CHECK(id.stamped && id.buildId == 0x0B59C11Au, "I5c Y/C id");
    CHECK(owenbIdentityRefusal(id, false).empty(), "I5c Y/C accepted");
    const OwenbBuildInfo* yc = recognisedOwenbBuild(0x0B59C11Au);
    CHECK(yc && !yc->labOnly, "I5c Y/C row");
    CHECK(yc && yc->lineage && std::strstr(yc->lineage, "c8b34604") != nullptr &&
              std::strstr(yc->lineage, "not fitted yet") == nullptr,
          "I5c Y/C lineage names the shipped RBF");
    CHECK(recognisedOwenbBuild(0x0B59C119u) == nullptr, "I5c dead 0B59C119 is not the Y/C row");
    f.plexob(false);
    id = readOwenbIdentity(f.reader(), 100);
    CHECK(!id.stamped && id.buildId == 0x0B000101u, "I5b unstamped id");
    CHECK(has(owenbIdentityRefusal(id, false), "lab-only"), "I5b unstamped still refused");

    // I6: a stamped id that is not in the table is still refused.
    f.plexob(true, 0x0B000201u);
    id = readOwenbIdentity(f.reader(), 100);
    CHECK(id.stamped && id.buildId == 0x0B000201u, "I6 stamped id read from word 0x58");
    CHECK(has(owenbIdentityRefusal(id, true), "not in the OB build table"), "I6 unknown stamped id refused");
    f.plexob(true, 0x48005D45u);
    id = readOwenbIdentity(f.reader(), 100);
    CHECK(has(owenbIdentityRefusal(id, true), "is not an OB (0x0B) build id"), "I6 a legacy CAPS id refused");
    f.plexob(false);
    f.w[owenb_id::kMpxBuildIdWord] = (0x12345678ull << 32) | 0x0B000201u;
    id = readOwenbIdentity(f.reader(), 100);
    CHECK(!id.stamped && id.buildId == 0x0B000101u, "I6 wrong tag: unstamped");
    f.w[owenb_id::kMpxBuildIdWord] = static_cast<uint64_t>(owenb_id::kMpxBuildIdTag) << 32;
    id = readOwenbIdentity(f.reader(), 100);
    CHECK(!id.stamped, "I6 tag with id 0: unstamped");
    CHECK(has(owenbIdentityRefusal(OwenbIdentity{}, true), "no control-block map"), "I6 not alive: refused");

    for (const OwenbBuildInfo& b : kOwenbBuilds)
        CHECK((b.id & owenb_id::kBuildIdFamilyMask) == owenb_id::kBuildIdFamily, "I7 every OB row is 0x0B");
    CHECK(recognisedFpgaBuild(0x0B000101u) == nullptr, "I7 OB ids are not legacy CAPS rows");

    unsetenv("MPX_OWENB_ALLOW_UNSTAMPED");
    CHECK(!owenbAllowUnstampedFromEnv(), "I8 unset: fail closed");
    setenv("MPX_OWENB_ALLOW_UNSTAMPED", "0", 1);
    CHECK(!owenbAllowUnstampedFromEnv(), "I8 0");
    setenv("MPX_OWENB_ALLOW_UNSTAMPED", "1", 1);
    CHECK(owenbAllowUnstampedFromEnv(), "I8 1");
    unsetenv("MPX_OWENB_ALLOW_UNSTAMPED");
}

static OwenbIdentity aliveId(uint32_t id = 0x0B000101u, bool stamped = false) {
    OwenbIdentity i;
    i.verdict = OwenbIdVerdict::Alive;
    i.magic = owenb_id::kMagic;
    i.version = owenb_id::kVersion;
    i.ringSize = owenb_id::kRingBytes;
    i.heartbeat0 = 7;
    i.heartbeat1 = 8;
    i.stamped = stamped;
    i.buildId = id;
    return i;
}
static OwenbIdentity verdictId(OwenbIdVerdict v) {
    OwenbIdentity i = aliveId();
    i.verdict = v;
    return i;
}

static void testGateOpen() {
    // G1: an OB open keeps the legacy gate closed (mutant MUT_OWENB_GATE_LEGACY_OPEN).
    Rig r;
    r.load("PlexOB", "_Utility/PlexOB_20261016.rbf");
    CHECK(r.toProbe(), "G1 PlexOB probes");
    CHECK(r.g.loadedFamily() == CoreFamily::PlexOwenb && r.g.ownsIdleInFabric(), "G1 family before open");
    CHECK(r.g.owenbProbeResult(aliveId(), "", r.now) == CoreGateEvent::Opened, "G1 alive opens");
    CHECK(r.g.state() == CoreGateState::Open, "G1 state Open");
    CHECK(r.g.owenbOpen(), "G1 owenbOpen");
    CHECK(!r.g.open(), "G1 open() stays false: no legacy PLXB/paint/doorbell/beat");
    CHECK(!r.g.idleErrorPaintAllowed(), "G1 no idle error paint");
    CHECK(r.g.lastOpenId() == 0 && r.g.lastCapsId() == 0, "G1 no legacy ids published");
    CHECK(r.g.buildId() == 0x0B000101u, "G1 build id");
    CHECK(has(r.logs(), "verified by ring identity"), "G1 log");
    CHECK(r.tick() == CoreGateEvent::None && r.g.owenbOpen(), "G1 stays open while nothing changes");
    // G3: a reload closes it.
    r.load("MENU", "menu.rbf");
    CHECK(r.tick() == CoreGateEvent::Closed, "G3 reload closes");
    CHECK(!r.g.owenbOpen() && !r.g.open() && !r.g.ownsIdleInFabric(), "G3 closed on menu");
    // ...and a legacy Plex core after it opens through CAPS as before.
    r.load("Plex", "_Utility/Plex_20260101.rbf");
    CHECK(r.toProbe(), "G3 legacy probes");
    CHECK(r.g.probeResult(true, 0x48005D45u, "", "480p", "row", r.now) == CoreGateEvent::Opened, "G3 CAPS open");
    CHECK(r.g.open() && !r.g.owenbOpen() && !r.g.ownsIdleInFabric(), "G3 legacy open");
}

static void testGateRetry() {
    {   // G4: a frozen ring retries with backoff, then gives up.
        Rig r;
        r.load("PlexOB", "PlexOB.rbf");
        int probes = 0;
        CoreGateEvent last = CoreGateEvent::None;
        for (int i = 0; i < 200 && last != CoreGateEvent::Rejected; ++i) {
            if (r.tick() == CoreGateEvent::ProbeWanted) {
                ++probes;
                last = r.g.owenbProbeResult(verdictId(OwenbIdVerdict::Stale), "", r.now);
            }
        }
        CHECK(last == CoreGateEvent::Rejected && probes == CoreGate::kMaxProbeAttempts, "G4 gives up after 5");
        CHECK(!r.g.owenbOpen() && !r.g.open() && !r.g.idleErrorPaintAllowed(), "G4 rejected, no paint");
        CHECK(r.g.ownsIdleInFabric(), "G4 still the OB core's idle (no daemon paint)");
        CHECK(has(r.logs(), "no live owenb ring"), "G4 log");
    }
    const OwenbIdVerdict vs[] = {OwenbIdVerdict::BadVersion, OwenbIdVerdict::BadGeometry};
    for (OwenbIdVerdict v : vs) {   // G5: refused at once.
        Rig r;
        r.load("PlexOB", "PlexOB.rbf");
        CHECK(r.toProbe(), "G5 probes");
        CHECK(r.g.owenbProbeResult(verdictId(v), "", r.now) == CoreGateEvent::Rejected, "G5 refused");
        CHECK(!r.g.owenbOpen(), "G5 closed");
    }
    Rig r;
    r.load("PlexOB", "PlexOB.rbf");
    CHECK(r.toProbe(), "G5 probes");
    CHECK(r.g.owenbProbeResult(aliveId(), "BUILD_ID lab-only", r.now) == CoreGateEvent::Rejected, "G5 lab-only");
    CHECK(!r.g.owenbOpen() && has(r.logs(), "lab-only"), "G5 lab-only closed");
    CHECK(r.g.owenbProbeResult(aliveId(), "", r.now) == CoreGateEvent::None, "G6 not Verifying: ignored");
    CHECK(!r.g.owenbOpen(), "G6 still closed");
}

struct TickCounts { int caps = 0, ring = 0, returned = 0, finish = 0, repaint = 0, obOpened = 0; };

static CoreGateTickHooks hooksFor(Rig& r, TickCounts& c, bool withRing) {
    CoreGateTickHooks h;
    h.observe = [&r] {
        r.now += 700000;
        r.o.nowUs = r.now;
        return r.o;
    };
    h.playing = [] { return false; };
    h.probe = [&r, &c] {
        ++c.caps;
        return r.g.probeResult(true, 0x48005D45u, "", "480p", "row", r.now);
    };
    h.coreReturned = [&c] { ++c.returned; };
    h.finishReturn = [&c] { ++c.finish; };
    h.repaint = [&c] { ++c.repaint; };
    h.log = [](const std::string&) {};
    if (withRing) {
        h.ringProbe = [&r, &c] {
            ++c.ring;
            return r.g.owenbProbeResult(aliveId(), "", r.now);
        };
        h.owenbOpened = [&c] { ++c.obOpened; };
    }
    return h;
}

static void testTick() {
    {   // T1: PlexOB: the ring probe, no CAPS, no per-core reset, no repaint.
        Rig r;
        r.load("PlexOB", "PlexOB.rbf");
        TickCounts c;
        const CoreGateTickHooks h = hooksFor(r, c, true);
        bool pending = false;
        for (int i = 0; i < 10; ++i)
            (void)coreGateTickStep(r.g, h, pending);
        CHECK(r.g.owenbOpen() && !r.g.open(), "T1 OB open");
        CHECK(c.ring == 1 && c.caps == 0, "T1 ring probe only");
        CHECK(c.returned == 0 && c.finish == 0 && c.repaint == 0, "T1 no coreReturned/finishReturn/repaint");
        CHECK(c.obOpened == 1 && !pending, "T1 owenbOpened once");
    }
    {   // T2: no ringProbe hook (a tool, an old caller): it never opens and never sends CAPS.
        Rig r;
        r.load("PlexOB", "PlexOB.rbf");
        TickCounts c;
        const CoreGateTickHooks h = hooksFor(r, c, false);
        bool pending = false;
        for (int i = 0; i < 10; ++i)
            (void)coreGateTickStep(r.g, h, pending);
        CHECK(!r.g.owenbOpen() && !r.g.open() && c.caps == 0 && c.repaint == 0, "T2 no hook: closed, no CAPS");
    }
    {   // T3: legacy unchanged: CAPS, coreReturned, finishReturn and repaint once.
        Rig r;
        r.load("Plex", "_Utility/Plex_20260101.rbf");
        TickCounts c;
        const CoreGateTickHooks h = hooksFor(r, c, true);
        bool pending = false;
        for (int i = 0; i < 10; ++i)
            (void)coreGateTickStep(r.g, h, pending);
        CHECK(r.g.open() && !r.g.owenbOpen(), "T3 legacy open");
        CHECK(c.caps == 1 && c.ring == 0, "T3 CAPS only");
        CHECK(c.returned == 1 && c.finish == 1 && c.repaint == 1 && c.obOpened == 0, "T3 legacy return sequence");
    }
}

static void testBackend() {
    using B = VideoBackend;
    CHECK(sessionVideoBackend(B::OwenbRing, false) == B::OwenbRing, "S1 env override on any core");
    CHECK(sessionVideoBackend(B::FpgaH264, true) == B::OwenbRing, "S2 PlexOB core picks OwenbRing");
    CHECK(sessionVideoBackend(B::LegacySoftware, true) == B::OwenbRing, "S2 over legacy-software");
    CHECK(sessionVideoBackend(B::Unselected, true) == B::OwenbRing, "S2 over unselected");
    CHECK(sessionVideoBackend(B::FpgaH264, false) == B::FpgaH264, "S3 legacy core: configured");
    CHECK(sessionVideoBackend(B::Unselected, false) == B::Unselected, "S3 unselected stays refused");
    CHECK(!legacyMenuReloadApplies(B::OwenbRing), "S5 PlexOB stays on the loaded core");
    CHECK(legacyMenuReloadApplies(B::FpgaH264), "S5 a legacy core may reload");
    CHECK(legacyMenuReloadApplies(sessionVideoBackend(B::FpgaH264, true)) == false,
          "S5 a PlexOB core does not plan a Utility reload");
    setenv("MPX_VIDEO_BACKEND", "owenb-ring", 1);
    CHECK(configuredVideoBackend() == B::OwenbRing, "S4 owenb-ring configured");
    unsetenv("MPX_VIDEO_BACKEND");
    CHECK(configuredVideoBackend() == B::Unselected, "S4 unset");
}

static void put64(int fd, uint64_t off, uint64_t v) { CHECK(::pwrite(fd, &v, 8, off) == 8, "M pwrite"); }

static void testMap() {
    char path[] = "/tmp/test_owenb_core_gate_mem.XXXXXX";
    const int fd = ::mkstemp(path);
    CHECK(fd >= 0, "M0 sparse file");
    if (fd < 0)
        return;
    CHECK(::ftruncate(fd, H264RING_RING_BYTE) == 0, "M0 size to the ring base");
    const uint64_t c = owenb_id::kCtrlPhys;
    put64(fd, c + 0, H264RING_MAGIC);
    put64(fd, c + 8, H264RING_VERSION);
    put64(fd, c + 16, static_cast<uint64_t>(H264RING_RING_SIZE) << 32);
    put64(fd, c + 24, 40);
    put64(fd, c + owenb_id::kMpxBuildIdWord * 8, 0);
    const uint64_t sentinel = 0xA5A5A5A55A5A5A5Aull;
    put64(fd, owenb_id::kIdleFbPhys, sentinel);
    CHECK(::fchmod(fd, 0444) == 0, "M0 file mode 0444");

    OwenbCtrlPageMap m;
    CHECK(m.open("/nonexistent/mem") < 0 && !m.mapped(), "M1 missing path: no map");
    CHECK(m.open(path) == 0 && m.mapped(), "M1 read-only open + PROT_READ map");
    const OwenbCtrlReader rd = m.reader();
    CHECK(rd.read(0) == H264RING_MAGIC && rd.read(3) == 40, "M1 reads the ctrl page");
    CHECK(rd.read(owenb_id::kCtrlBytes / 8) == 0, "M1 nothing past the 4 KiB page");
    m.close();

    // M2: frozen file heartbeat -> Stale -> retry; then a writer advances it -> Alive -> lab-only refusal.
    Rig r;
    r.load("PlexOB", "PlexOB.rbf");
    CHECK(r.toProbe(), "M2 probes");
    CHECK(owenbGateRingProbe(r.g, path, 30, r.now, true) == CoreGateEvent::None, "M2 frozen: retry");
    CHECK(has(r.logs(), "heartbeat frozen"), "M2 log says frozen");
    std::atomic<bool> stop{false};
    std::thread beat([&] {
        uint64_t hb = 41;
        while (!stop.load()) {
            (void)::pwrite(fd, &hb, 8, c + 24);
            ++hb;
            ::usleep(2000);
        }
    });
    CHECK(r.toProbe(), "M3 probes again");
    CHECK(owenbGateRingProbe(r.g, path, 200, r.now, false) == CoreGateEvent::Rejected, "M3 unstamped: lab-only");
    Rig r2;
    r2.load("PlexOB", "PlexOB.rbf");
    CHECK(r2.toProbe(), "M4 probes");
    CHECK(owenbGateRingProbe(r2.g, path, 200, r2.now, true) == CoreGateEvent::Opened, "M4 alive + lab opt-in opens");
    CHECK(r2.g.owenbOpen() && !r2.g.open(), "M4 OB open, legacy closed");
    stop.store(true);
    beat.join();
    uint64_t v = 0;
    CHECK(::pread(fd, &v, 8, owenb_id::kIdleFbPhys) == 8 && v == sentinel, "M5 idle FB 0x31600000 untouched");
    ::close(fd);
    ::unlink(path);
}

static std::string readFile(const std::string& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string body(const std::string& src, const char* head) {
    const size_t a = src.find(head);
    if (a == std::string::npos)
        return {};
    const size_t b = src.find("\n}\n", a);
    return b == std::string::npos ? std::string() : src.substr(a, b - a);
}

// P: paintIdle returns on ownsIdleInFabric() before anything consumes gateRepaintOnce_ (or arms the idle FB).
static bool paintOrderOk(const std::string& b) {
    // rc3 paintIdle returns from the ownsIdleInFabric brace (after obPlexUiPublish),
    // still before the repaint latch and the idle FB arm.
    const size_t own = b.find("if (CoreGate::process().ownsIdleInFabric())");
    const size_t ret = own == std::string::npos ? std::string::npos : b.find("\n        return;\n", own);
    const size_t latch = b.find("gateRepaintOnce_.exchange(");
    const size_t arm = b.find("r1IdleFbReady()");
    return own != std::string::npos && ret != std::string::npos && latch != std::string::npos &&
           arm != std::string::npos && ret < latch && own < arm;
}

static void testSource(bool mutatePaintOrder) {
    const std::string src = readFile(std::string(MPX_SOURCE_ROOT) + "/arm/misterplexd/media_player.cpp");
    CHECK(!src.empty(), "P0 media_player.cpp read");
    std::string paint = body(src, "void MediaPlayer::paintIdle() {");
    if (mutatePaintOrder) {
        // The mutant: the early return moved after the latch is taken.
        const std::string ret =
            "    if (CoreGate::process().ownsIdleInFabric()) {\n"
            "        obPlexUiPublish();   // OB port (MPX_PLEXUI=1): PlexUI is a full-screen overlay surface\n"
            "        return;\n"
            "    }\n";
        const size_t at = paint.find(ret);
        if (at != std::string::npos) {
            const size_t beginNl = paint.rfind('\n', at);
            const size_t begin = beginNl == std::string::npos ? 0 : beginNl + 1;
            const size_t ret = paint.find("return;", at);
            const size_t brace = ret == std::string::npos ? std::string::npos : paint.find('}', ret);
            const size_t endNl = brace == std::string::npos ? std::string::npos : paint.find('\n', brace);
            if (endNl != std::string::npos) {
                const std::string block = paint.substr(begin, endNl + 1 - begin);
                paint.erase(begin, block.size());
                const size_t l = paint.find("gateRepaintOnce_.exchange(");
                const size_t eol = l == std::string::npos ? std::string::npos : paint.find('\n', l);
                if (eol != std::string::npos)
                    paint.insert(eol + 1, block);
            }
        }
    }
    CHECK(paintOrderOk(paint), "P1 paintIdle: OB early return before gateRepaintOnce_ and the idle FB arm");
    const std::string play = body(src, "bool MediaPlayer::playInternal(");
    const size_t route = play.find("if (sessionBackend() == VideoBackend::OwenbRing)\n        return playOwenbRoute(");
    const size_t legacy = play.find("if (!CoreGate::process().open()) {");
    CHECK(route != std::string::npos && legacy != std::string::npos && route < legacy,
          "P2 playInternal: the OB route comes before the legacy gate check");
    const std::string ob = body(src, "bool MediaPlayer::playOwenbRoute(");
    CHECK(!ob.empty() && ob.find("fpga_.") == std::string::npos && ob.find("fb_.") == std::string::npos &&
              ob.find("paint") == std::string::npos,
          "P3 the OB route touches no legacy FPGA/fb path");
    const std::string tick = body(src, "void MediaPlayer::coreGateTick() {");
    CHECK(has(tick, "h.ringProbe = [] {") && has(tick, "owenbGateRingProbe(CoreGate::process(), MPX_DEVMEM_PATH"),
          "P4 the daemon's ring probe hook");
}

int main(int argc, char** argv) {
    const bool mutatePaint = argc > 1 && std::strcmp(argv[1], "--mutate-paint-order") == 0;
    unsetenv("MPX_VIDEO_BACKEND");
    unsetenv("MPX_OWENB_ALLOW_UNSTAMPED");
    testNames();
    testIdentity();
    testGateOpen();
    testGateRetry();
    testTick();
    testBackend();
    testMap();
    testSource(mutatePaint);
    if (fails) {
        std::fprintf(stderr, "test_owenb_core_gate: %d of %d checks FAILED\n", fails, checks);
        return 1;
    }
    std::printf("test_owenb_core_gate: %d checks OK\n", checks);
    return 0;
}
