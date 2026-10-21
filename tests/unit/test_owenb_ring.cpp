// MPX OwenbRing host tests: OwenbRing (host/libmisterplex/owenb_ring.cpp) on top of the vendored owenb h264ring
// lib, attached to a HeapDdrWindow that a fake core drives.
// The fake core is a C++ port of owenb321's hps/tests/test_ring.c fake core (MIT, (c) 2026 owenb321 and
// contributors; third_party/owenb-h264ring/LICENSE). It covers the boot rule, the nonce/ack, the read
// index and the seqlocked status words. Records are parsed here from the bytes the fake core consumed,
// independently of the library.
// Mutant MUT_OWENB_SKIP_HEARTBEAT (probe skips the heartbeat) must FAIL case P4 (frozen heartbeat -> Stale).
#include "libmisterplex/owenb_ring.hpp"
#include "libmisterplex/video_backend.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

using namespace misterplex;
using namespace misterplex::owenb;
using h264stream::ControlResult;
using h264stream::PushResult;

namespace {
int g_fails = 0, g_checks = 0;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_fails;                                                                             \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                                       \
            std::printf(__VA_ARGS__);                                                              \
            std::printf("\n");                                                                     \
        }                                                                                          \
    } while (0)

constexpr uint32_t kRing = 64u * 1024u;

struct FakeCore {
    explicit FakeCore(HeapDdrWindow& w) : win(w) { win.onStep = [this] { step(); }; }
    HeapDdrWindow& win;
    bool alive = true, frozen = false, magic = true, consume = true, error = false;
    uint32_t version = H264RING_VERSION, reportRing = kRing;
    bool booted = false, sess = false;
    uint32_t seen = 0, rw = 0, seq = 0, ack = 0;
    std::vector<uint8_t> log;   // bytes consumed since the session's start index

    uint64_t* c() { return win.ctrlWords(); }
    uint32_t lo(unsigned i) { return static_cast<uint32_t>(c()[i]); }
    void status() {
        if (!alive)
            return;
        if (!frozen)
            seq++;
        c()[0] = magic ? H264RING_MAGIC : 0x0123456789ABCDEFull;
        c()[1] = version | (32ull << 16) | (32ull << 32);
        c()[2] = 0x31800000ull | (static_cast<uint64_t>(reportRing) << 32);
        c()[3] = seq | (static_cast<uint64_t>(H264RING_MAX_PAYLOAD) << 32);
        c()[4] = !sess ? H264RING_S_NO_SESSION : error ? H264RING_S_ERROR : H264RING_S_PLAYING;
        c()[5] = rw | (static_cast<uint64_t>(ack) << 32);
        c()[31] = seq;
    }
    void step() {
        const uint32_t wi = lo(H264RING_W_WRITE_INDEX), nonce = lo(H264RING_W_NONCE);
        if (!booted) {
            seen = nonce;   // an earlier program's nonce: ignored (the boot rule)
            booted = true;
        } else if (nonce != seen) {
            seen = nonce;
            sess = true;
            rw = wi & ~15u;
            ack = nonce;
            log.clear();
        } else if (sess && consume) {
            const uint64_t* r = win.ringWords();
            while (rw != wi) {
                log.push_back(static_cast<uint8_t>(r[(rw % kRing) / 8] >> (8 * (rw % 8))));
                rw++;
            }
        }
        status();
    }
};

struct Record {
    unsigned type = 0, flags = 0, seq = 0;
    uint32_t len = 0;
    uint64_t pts = 0;
    unsigned sync = 0;
    std::vector<uint8_t> payload;
};

uint64_t le64(const std::vector<uint8_t>& b, size_t o) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | b[o + static_cast<size_t>(i)];
    return v;
}

// Parses the records the core consumed, skipping PAD records. Independent of the library.
std::vector<Record> parse(const std::vector<uint8_t>& b) {
    std::vector<Record> out;
    size_t o = 0;
    while (o + 16 <= b.size()) {
        Record r;
        const uint64_t w0 = le64(b, o), w1 = le64(b, o + 8);
        r.type = w0 & 0xFF;
        r.flags = (w0 >> 8) & 0xFF;
        r.seq = (w0 >> 16) & 0xFFFF;
        r.len = static_cast<uint32_t>(w0 >> 32);
        r.pts = w1 & ((1ull << 33) - 1);
        r.sync = static_cast<unsigned>(w1 >> 48);
        const size_t n = 16 + ((static_cast<size_t>(r.len) + 15) & ~static_cast<size_t>(15));
        if (o + n > b.size())
            break;
        if (r.type != H264RING_T_PAD) {
            r.payload.assign(b.begin() + static_cast<long>(o + 16), b.begin() + static_cast<long>(o + 16 + r.len));
            out.push_back(r);
        }
        o += n;
    }
    return out;
}

// An access unit with no slice (AUD plus filler), which the CAVLC rewrite passes through unchanged.
std::vector<uint8_t> testAu(size_t fill) {
    std::vector<uint8_t> au = {0, 0, 0, 1, 0x09, 0xF0, 0, 0, 0, 1, 0x0C};
    for (size_t i = 0; i < fill; ++i)
        au.push_back(static_cast<uint8_t>(0xFF));
    au.push_back(0x80);
    return au;
}

struct Rig {
    Rig() : win(kRing), core(win), ring(win, 250) { core.status(); }
    HeapDdrWindow win;
    FakeCore core;
    OwenbRing ring;
    PushResult au(uint64_t sid, uint32_t seq, const std::vector<uint8_t>& b, int64_t pts = -1, bool eos = false,
                  bool disc = false) {
        h264stream::AccessUnitView v;
        v.session_id = sid;
        v.seq = seq;
        v.annexb = b.data();
        v.len = b.size();
        v.pts90k = pts;
        v.end_of_stream = eos;
        v.discontinuity = disc;
        return ring.pushAccessUnit(v);
    }
    PushResult pcm(uint64_t sid, const std::vector<uint8_t>& b, int64_t pts) {
        h264stream::PcmView v;
        v.session_id = sid;
        v.s16le = b.data();
        v.len = b.size();
        v.pts90k = pts;
        return ring.pushPcm(v);
    }
    void drain() { win.sleepUs(1); }
};

// P1-P6: descriptor, magic, version and heartbeat detection.
void testProbe() {
    {   // P1: magic missing -> NoCore (another core's words)
        Rig r;
        r.core.magic = false;
        r.core.status();
        CHECK(r.ring.probe(50).verdict == ProbeVerdict::NoCore, "P1 no magic -> NoCore");
        CHECK(r.ring.begin(1) == ControlResult::Fatal, "P1 begin without a core is Fatal");
    }
    {   // P2: major version 2 -> BadVersion; an older minor (0x100) -> BadVersion
        Rig r;
        r.core.version = 0x200;
        r.core.status();
        RingProbe p = r.ring.probe(50);
        CHECK(p.verdict == ProbeVerdict::BadVersion && p.version == 0x200, "P2 0x200 -> %s", toString(p.verdict));
        r.core.version = 0x100;
        r.core.status();
        CHECK(r.ring.probe(50).verdict == ProbeVerdict::BadVersion, "P2 minor older than 0x101 -> BadVersion");
        r.core.version = 0x10F;   // a newer minor is compatible
        r.core.status();
        CHECK(r.ring.probe(50).verdict == ProbeVerdict::Alive, "P2 newer minor 0x10F -> Alive");
    }
    {   // P3: geometry differs from the window -> BadGeometry
        Rig r;
        r.core.reportRing = 2 * kRing;
        r.core.status();
        RingProbe p = r.ring.probe(50);
        CHECK(p.verdict == ProbeVerdict::BadGeometry && p.ring_size == 2 * kRing, "P3 -> %s", toString(p.verdict));
        CHECK(r.ring.lastErrno() == -EPROTONOSUPPORT, "P3 errno %d", r.ring.lastErrno());
    }
    {   // P4: words present but the heartbeat is frozen -> Stale (the mutant fails here)
        Rig r;
        r.core.frozen = true;
        RingProbe p = r.ring.probe(50);
        CHECK(p.verdict == ProbeVerdict::Stale, "P4 frozen heartbeat -> %s", toString(p.verdict));
        CHECK(r.ring.lastErrno() == -ETIMEDOUT, "P4 errno %d", r.ring.lastErrno());
        r.core.frozen = false;
        CHECK(r.ring.probe(50).verdict == ProbeVerdict::Alive, "P4 heartbeat resumes -> Alive");
    }
    {   // P5: alive; probe writes no program word
        Rig r;
        RingProbe p = r.ring.probe(50);
        CHECK(p.verdict == ProbeVerdict::Alive && p.magic == H264RING_MAGIC && p.version == H264RING_VERSION,
              "P5 alive -> %s", toString(p.verdict));
        bool clean = true;
        for (unsigned w = H264RING_PROG_WORD; w < H264RING_PROG_WORD + 8; ++w)
            clean = clean && r.win.ctrlWords()[w] == 0;
        CHECK(clean, "P5 probe wrote a program word");
    }
    {   // P6: an unmapped window -> NoMap
        DevMemDdrWindow w;
        OwenbRing ring(w);
        CHECK(ring.probe(10).verdict == ProbeVerdict::NoMap, "P6 unmapped -> NoMap");
        CHECK(std::strcmp(toString(ProbeVerdict::Stale), "Stale") == 0, "P6 toString");
    }
}

// S1-S4: session start, the ack, wrong session or seq, and an OSD Reset.
void testSession() {
    Rig r;
    CHECK(r.ring.begin(7) == ControlResult::Ok, "S1 begin: %s", r.ring.lastError().c_str());
    const uint32_t nonce = r.core.lo(H264RING_W_NONCE);
    CHECK(nonce != 0 && r.core.ack == nonce && r.core.sess, "S1 the core acked nonce %u (ack %u)", nonce, r.core.ack);
    CHECK(r.ring.begin(8) == ControlResult::ActiveSession, "S1 begin twice -> ActiveSession");
    CHECK(r.ring.status().active && r.ring.status().session_id == 7, "S1 telemetry active");

    const auto a = testAu(20);
    CHECK(r.au(9, 0, a) == PushResult::Fatal, "S2 wrong session -> Fatal");
    CHECK(r.au(7, 3, a) == PushResult::Desync, "S2 seq 3 at 0 -> Desync");
    CHECK(r.ring.status().desync_count == 1 && r.ring.status().last_bad_seq == 3, "S2 desync telemetry");
    CHECK(r.au(7, 0, {}) == PushResult::Fatal, "S2 empty AU -> Fatal");
    CHECK(r.ring.flush(9) == ControlResult::NoSession && r.ring.pause(9) == ControlResult::NoSession,
          "S2 control on the wrong session -> NoSession");

    CHECK(r.au(7, 0, a) == PushResult::Ok, "S3 first AU ok: %s", r.ring.lastError().c_str());
    r.core.ack ^= 0x5A5Au;   // an OSD Reset: the core now runs another nonce
    r.core.status();
    CHECK(r.au(7, 1, a) == PushResult::Fatal, "S3 lost session -> Fatal");
    CHECK(r.ring.lastErrno() == -ENOTCONN && !r.ring.status().active, "S3 ENOTCONN, inactive (%d)", r.ring.lastErrno());
    CHECK(r.ring.begin(7) == ControlResult::Ok, "S3 a new begin recovers");

    r.core.error = true;
    r.core.status();
    CHECK(r.au(7, 0, a) == PushResult::Fatal && r.ring.lastErrno() == -EPROTO, "S4 core ERROR -> Fatal EPROTO");
}

// W1-W4: AU and PCM records as the core reads them.
void testRecords() {
    Rig r;
    CHECK(r.ring.begin(1) == ControlResult::Ok, "W begin");
    const auto a0 = testAu(40), a1 = testAu(3), a2 = testAu(17);
    h264stream::NalView nv;
    nv.session_id = 1;
    nv.seq = 0;
    nv.nal_type = 5;
    nv.annexb = a0.data();
    nv.len = a0.size();
    CHECK(r.ring.pushNal(nv) == PushResult::Ok, "W1 pushNal (one AU): %s", r.ring.lastError().c_str());
    CHECK(r.au(1, 1, a1, 90000) == PushResult::Ok, "W2 AU with pts");
    std::vector<uint8_t> pcm(960 * 4);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<uint8_t>(i * 7);
    CHECK(r.pcm(1, pcm, 90000) == PushResult::Ok, "W3 PCM 960 frames");
    CHECK(r.au(1, 2, a2, 93003, true) == PushResult::Ok, "W4 EOS AU");
    std::vector<uint8_t> odd(6, 1);
    CHECK(r.pcm(1, odd, -1) == PushResult::Fatal && r.ring.lastErrno() == -EINVAL, "W3 6-byte PCM -> Fatal EINVAL");
    CHECK(r.pcm(1, {}, -1) == PushResult::Fatal, "W3 empty PCM -> Fatal");
    CHECK(r.ring.status().active, "W3 a bad PCM keeps the session");
    r.drain();
    const auto recs = parse(r.core.log);
    CHECK(recs.size() == 4, "W records %zu", recs.size());
    if (recs.size() == 4) {
        CHECK(recs[0].type == H264RING_T_VIDEO_AU && recs[0].payload == a0 && !(recs[0].flags & H264RING_F_PTS_VALID),
              "W1 AU record: type %u flags %u", recs[0].type, recs[0].flags);
        CHECK(recs[1].payload == a1 && (recs[1].flags & H264RING_F_PTS_VALID) && recs[1].pts == 90000,
              "W2 pts %llu flags %u", static_cast<unsigned long long>(recs[1].pts), recs[1].flags);
        CHECK(recs[2].type == H264RING_T_AUDIO && recs[2].payload == pcm && recs[2].pts == 90000 &&
                  (recs[2].flags & H264RING_F_PTS_VALID),
              "W3 audio record type %u len %u", recs[2].type, recs[2].len);
        CHECK((recs[3].flags & H264RING_F_END_OF_STREAM) && recs[3].pts == 93003, "W4 EOS flags %u", recs[3].flags);
        bool sync = true;
        for (const auto& x : recs)
            sync = sync && x.sync == H264RING_SYNC;
        CHECK(sync, "W header sync word");
    }
    const auto t = r.ring.status();
    CHECK(t.producer_seq == 3 && t.nal_accepted == 3 && t.bytes_accepted == a0.size() + a1.size() + a2.size(),
          "W telemetry seq %u", t.producer_seq);
}

// F1-F2: Full, then a wrap through a PAD record.
void testFull() {
    Rig r;
    CHECK(r.ring.begin(2) == ControlResult::Ok, "F begin");
    r.core.consume = false;
    const auto big = testAu(7000);
    uint32_t seq = 0;
    PushResult pr = PushResult::Ok;
    for (int i = 0; i < 64 && pr == PushResult::Ok; ++i) {
        pr = r.au(2, seq, big);
        if (pr == PushResult::Ok)
            ++seq;
    }
    CHECK(pr == PushResult::Full && seq >= 7 && seq <= 9, "F1 Full after %u AUs (%s)", seq, h264stream::toString(pr));
    CHECK(r.ring.status().overrun_count == 1 && r.ring.status().ring_level_bytes > kRing - 8192,
          "F1 overrun %llu level %zu", static_cast<unsigned long long>(r.ring.status().overrun_count),
          r.ring.status().ring_level_bytes);
    r.core.consume = true;
    r.drain();
    for (int i = 0; i < 10; ++i) {   // wraps the ring at least once
        CHECK(r.au(2, seq, big) == PushResult::Ok, "F2 AU %u after the drain", seq);
        ++seq;
        r.drain();
    }
    const auto recs = parse(r.core.log);
    bool same = recs.size() == seq;
    for (const auto& x : recs)
        same = same && x.payload == big;
    CHECK(same, "F2 %zu records intact across the wrap (want %u)", recs.size(), seq);
}

// C1-C4: pause/resume, attenuation, flush, end.
void testControl() {
    Rig r;
    CHECK(r.ring.begin(3) == ControlResult::Ok, "C begin");
    const unsigned pw = H264RING_W_PAUSE;
    CHECK(r.ring.pause(3) == ControlResult::Ok && (r.core.lo(pw) & 1u) && r.ring.status().paused, "C1 pause bit");
    CHECK(r.ring.setAttenuation(3, 40) == ControlResult::Ok && ((r.core.lo(pw) >> 8) & 0xFF) == 40 &&
              (r.core.lo(pw) & 1u),
          "C2 atten 40 kept with the pause: 0x%x", r.core.lo(pw));
    CHECK(r.ring.resume(3) == ControlResult::Ok && !(r.core.lo(pw) & 1u) && ((r.core.lo(pw) >> 8) & 0xFF) == 40,
          "C1 resume clears only the pause: 0x%x", r.core.lo(pw));
    CHECK(r.ring.setAttenuation(3, 256) == ControlResult::Fatal, "C2 atten 256 -> Fatal");
    CHECK((r.core.lo(pw) & H264RING_CTL_RESERVED) == 0, "C2 reserved bits 0");

    const auto a = testAu(30);
    CHECK(r.au(3, 0, a) == PushResult::Ok && r.au(3, 1, a) == PushResult::Ok, "C3 two AUs");
    const uint32_t n0 = r.core.lo(H264RING_W_NONCE);
    CHECK(r.ring.flush(3) == ControlResult::Ok, "C3 flush: %s", r.ring.lastError().c_str());
    const uint32_t n1 = r.core.lo(H264RING_W_NONCE);
    CHECK(n1 != n0 && r.core.ack == n1, "C3 flush = a new nonce, acked (%u -> %u)", n0, n1);
    CHECK(r.ring.status().consumer_seq == 2 && r.ring.status().active, "C3 consumer_seq follows");
    CHECK(r.au(3, 2, a) == PushResult::Ok, "C3 the seq continues after flush");

    r.ring.pause(3);
    const uint32_t nb = r.core.lo(H264RING_W_NONCE);
    CHECK(r.ring.end(3) == ControlResult::Ok && !r.ring.status().active, "C4 end");
    CHECK(r.core.lo(H264RING_W_NONCE) == nb && !(r.core.lo(pw) & 1u), "C4 end holds the picture (no flush) and unpauses");
    CHECK(r.ring.end(3) == ControlResult::NoSession, "C4 end twice -> NoSession");
    CHECK(r.ring.begin(4) == ControlResult::Ok && !(r.core.lo(pw) & 1u), "C4 a new session starts unpaused");
}

// T1: toPts90k. B1: MPX_VIDEO_BACKEND parsing.
void testPtsAndBackend() {
    CHECK(toPts90k(1000, 1, 1000) == 90000, "T1 ms");
    CHECK(toPts90k(12345, 1, 90000) == 12345, "T1 90k identity");
    CHECK(toPts90k(1001, 1001, 30000) == 3006003, "T1 ntsc %lld",
          static_cast<long long>(toPts90k(1001, 1001, 30000)));
    CHECK(toPts90k(1, 1001, 30000) == 3003, "T1 one NTSC frame");
    CHECK(toPts90k(-5, 1, 1000) == h264stream::kNoPts90k && toPts90k(5, 0, 1) == h264stream::kNoPts90k, "T1 none");
    const int64_t wrap = toPts90k((1ll << 33) + 7, 1, 90000);
    CHECK(wrap == 7, "T1 33-bit wrap %lld", static_cast<long long>(wrap));
    CHECK(toPts90k(10000000000ll, 1, 1000000) == (900000000ll % (1ll << 33)), "T1 us, large");
    {   // against a 128-bit reference (host only), across time bases and magnitudes
        uint64_t x = 0x9E3779B97F4A7C15ull;
        int bad = 0;
        for (int i = 0; i < 20000; ++i) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            const int64_t ts = static_cast<int64_t>(x >> (1 + (i % 40)));
            const uint32_t num = 1u + static_cast<uint32_t>((x >> 20) % (i % 2 ? 1001u : 0xFFFFFFFEu));
            const uint32_t den = 1u + static_cast<uint32_t>((x >> 8) % (i % 3 ? 90000u : 0xFFFFFFFEu));
            const unsigned __int128 ref =
                static_cast<unsigned __int128>(static_cast<uint64_t>(ts)) * num * 90000u / den;
            if (toPts90k(ts, num, den) != static_cast<int64_t>(static_cast<uint64_t>(ref) & ((1ull << 33) - 1)))
                ++bad;
        }
        CHECK(bad == 0, "T1 %d of 20000 differ from the 128-bit reference", bad);
    }

    CHECK(parseVideoBackend("owenb-ring") == VideoBackend::OwenbRing, "B1 owenb-ring parses");
    CHECK(parseVideoBackend("fpga-h264") == VideoBackend::FpgaH264, "B1 fpga-h264");
    CHECK(parseVideoBackend("legacy-software") == VideoBackend::LegacySoftware, "B1 legacy");
    CHECK(parseVideoBackend(nullptr) == VideoBackend::Unselected && parseVideoBackend("x") == VideoBackend::Unselected,
          "B1 unknown");
    setenv("MPX_VIDEO_BACKEND", "owenb-ring", 1);
    CHECK(configuredVideoBackend() == VideoBackend::OwenbRing, "B1 owenb-ring wired (T3): OwenbRing");
    setenv("MPX_VIDEO_BACKEND", "fpga-h264", 1);
    CHECK(configuredVideoBackend() == VideoBackend::FpgaH264, "B1 fpga-h264 unchanged");
    unsetenv("MPX_VIDEO_BACKEND");
}

// D1-D3: DevMemDdrWindow on a sparse file standing in for /dev/mem: the physical offsets, the lock, and no
// mapping at 0x31600000 (the idle FB).
void testDevMemWindow() {
    char dir[] = "/tmp/owenb_ring_test.XXXXXX";
    if (!mkdtemp(dir)) {
        CHECK(false, "D mkdtemp");
        return;
    }
    const std::string mem = std::string(dir) + "/mem", lock = std::string(dir) + "/lock";
    {
        FILE* f = std::fopen(mem.c_str(), "w");
        CHECK(f && ftruncate(fileno(f), static_cast<off_t>(0x32000000u)) == 0, "D sparse file");
        if (f)
            std::fclose(f);
    }
    {
        DevMemDdrWindow w;
        CHECK(w.open(mem.c_str(), lock.c_str()) == 0 && w.mapped(), "D1 open the stand-in");
        w.ctrl()[0] = H264RING_MAGIC;
        w.ring()[0] = 0x1122334455667788ull;
        DevMemDdrWindow w2;
        CHECK(w2.open(mem.c_str(), lock.c_str()) == -EBUSY && !w2.mapped(), "D2 a second feeder -> EBUSY");
        OwenbRing ring(w);
        CHECK(ring.probe(5).verdict == ProbeVerdict::BadVersion,
              "D1 magic visible to probe through the window");
    }
    {
        FILE* f = std::fopen(mem.c_str(), "r");
        uint64_t c0 = 0, r0 = 0, fb = 1;
        if (f) {
            std::fseek(f, static_cast<long>(H264RING_CTRL_BYTE), SEEK_SET);
            CHECK(std::fread(&c0, 8, 1, f) == 1, "D read ctrl");
            std::fseek(f, static_cast<long>(H264RING_RING_BYTE), SEEK_SET);
            CHECK(std::fread(&r0, 8, 1, f) == 1, "D read ring");
            std::fseek(f, 0x31600000l, SEEK_SET);
            CHECK(std::fread(&fb, 8, 1, f) == 1, "D read idle fb");
            std::fclose(f);
        }
        CHECK(c0 == H264RING_MAGIC && r0 == 0x1122334455667788ull, "D1 ctrl at 0x31400000, ring at 0x31800000");
        CHECK(fb == 0, "D3 nothing written at 0x31600000");
    }
    {
        DevMemDdrWindow w;   // the lock was released by the first window's close
        CHECK(w.open(mem.c_str(), lock.c_str()) == 0, "D2 lock released on close");
        DevMemDdrWindow missing;
        CHECK(missing.open((std::string(dir) + "/absent").c_str(), "") == -ENOENT, "D1 a missing device -> ENOENT");
    }
    unlink(mem.c_str());
    unlink(lock.c_str());
    rmdir(dir);
}
}  // namespace

int main() {
    testProbe();
    testSession();
    testRecords();
    testFull();
    testControl();
    testPtsAndBackend();
    testDevMemWindow();
    std::printf("test_owenb_ring: %d/%d checks passed\n", g_checks - g_fails, g_checks);
    if (g_fails) {
        std::printf("test_owenb_ring: FAIL\n");
        return 1;
    }
    std::printf("test_owenb_ring: ALL GREEN\n");
    return 0;
}
