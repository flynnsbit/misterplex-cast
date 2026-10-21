#include "libmisterplex/h264_nal_dispatch.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace misterplex::h264stream;

namespace {
int fails = 0;
#define CHECK(x)                                                                                  \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                     \
            ++fails;                                                                               \
        }                                                                                          \
    } while (0)

std::vector<uint8_t> nal(uint8_t type, std::initializer_list<uint8_t> payload) {
    std::vector<uint8_t> v{0, 0, 0, 1, static_cast<uint8_t>(0x60 | (type & 0x1f))};
    v.insert(v.end(), payload.begin(), payload.end());
    return v;
}

std::vector<uint8_t> readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    CHECK(in.good());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct FlakyProducer final : public IBitstreamProducer {
    CopyRingBitstreamProducer inner{1024};
    int full_before_ok = 0;
    ControlResult begin(uint64_t s) override { return inner.begin(s); }
    PushResult pushNal(const NalView& n) override {
        if (full_before_ok > 0) {
            --full_before_ok;
            return PushResult::Full;
        }
        return inner.pushNal(n);
    }
    ControlResult flush(uint64_t s) override { return inner.flush(s); }
    ControlResult end(uint64_t s) override { return inner.end(s); }
    ControlResult pause(uint64_t s) override { return inner.pause(s); }
    ControlResult resume(uint64_t s) override { return inner.resume(s); }
    Telemetry status() const override { return inner.status(); }
};
} // namespace

int main() {
    const auto sps = nal(7, {0x42, 0x00, 0x1e});
    const auto pps = nal(8, {0xce, 0x06});
    auto idr = nal(5, {0xaa, 0xbb, 0xcc});
    const auto p = nal(1, {0x11, 0x22});

    // Copy-on-push: producer owns bytes before returning.
    CopyRingBitstreamProducer ring(256);
    CHECK(ring.begin(10) == ControlResult::Ok);
    NalView v{10, 0, 5, idr.data(), idr.size()};
    CHECK(ring.pushNal(v) == PushResult::Ok);
    idr[5] ^= 0xff;
    const auto snap = ring.snapshot();
    CHECK(snap.size() == v.len);
    CHECK(snap[5] == 0xaa);
    CHECK(ring.begin(11) == ControlResult::ActiveSession);
    CHECK(ring.end(10) == ControlResult::Ok);

    // Source dispatcher pause contract (prod 735c8281; STREAM=1/F3 is lab-only, ticket
    // review/daemon/stream1-naldispatcher-pause-ticket-1005.md): a paused handleNal is
    // back-pressure (Full) and is neither parsed nor counted; resume does NOT replay SPS/PPS.
    CopyRingBitstreamProducer replayRing(4096);
    DispatchConfig cfg;
    cfg.max_full_retries = 0;
    cfg.sleep_ms = [](int) {};
    NalDispatcher dispatch(replayRing, cfg);
    CHECK(dispatch.begin(20) == ControlResult::Ok);
    CHECK(dispatch.handleNal(sps.data(), sps.size()) == PushResult::Ok);
    CHECK(dispatch.handleNal(pps.data(), pps.size()) == PushResult::Ok);
    CHECK(dispatch.handleNal(idr.data(), idr.size()) == PushResult::Ok);
    const uint64_t seenBeforePause = dispatch.stats().nal_seen;
    const uint64_t pushedBeforePause = dispatch.stats().nal_pushed;
    CHECK(dispatch.pause() == ControlResult::Ok);
    CHECK(dispatch.handleNal(p.data(), p.size()) == PushResult::Full);
    CHECK(dispatch.handleNal(sps.data(), sps.size()) == PushResult::Full);
    CHECK(dispatch.stats().nal_seen == seenBeforePause);
    CHECK(dispatch.stats().nal_pushed == pushedBeforePause);
    CHECK(dispatch.resume() == ControlResult::Ok);
    CHECK(dispatch.handleNal(p.data(), p.size()) == PushResult::Ok);
    CHECK(dispatch.stats().nal_pushed == pushedBeforePause + 1);
    CHECK(dispatch.stats().nal_dropped_paused == 0);
    CHECK(dispatch.stats().sps_replayed == 0);
    CHECK(dispatch.stats().pps_replayed == 0);
    CHECK(dispatch.end() == ControlResult::Ok);

    // Full is transient and retried; persistent Full escalates distinctly.
    FlakyProducer flaky;
    NalDispatcher retry(flaky, cfg);
    CHECK(retry.begin(30) == ControlResult::Ok);
    flaky.full_before_ok = 1;
    DispatchConfig retryCfg;
    retryCfg.max_full_retries = 2;
    retryCfg.sleep_ms = [](int) {};
    NalDispatcher retry2(flaky, retryCfg);
    CHECK(retry.end() == ControlResult::Ok);
    CHECK(retry2.begin(31) == ControlResult::Ok);
    CHECK(retry2.handleNal(sps.data(), sps.size()) == PushResult::Ok);
    CHECK(retry2.stats().full_retries == 1);
    CHECK(retry2.end() == ControlResult::Ok);

    CopyRingBitstreamProducer tiny(8);
    CHECK(tiny.begin(40) == ControlResult::Ok);
    NalView big{40, 0, 5, idr.data(), idr.size()};
    CHECK(tiny.pushNal(big) == PushResult::Ok);
    NalView tooMuch{40, 1, 1, p.data(), p.size()};
    CHECK(tiny.pushNal(tooMuch) == PushResult::Full);
    CHECK(tiny.status().overrun_count == 1);
    CHECK(tiny.consumeBytes(4096) != 0);
    CHECK(tiny.consumeBytes(1) == 0);
    CHECK(tiny.status().underrun_count == 1);
    const auto pressure = tiny.status();

    // Ring-space reuse / wrap contract: after the consumer advances, producer
    // can fill freed space without forcing a session reset.
    CHECK(tiny.pushNal(tooMuch) == PushResult::Ok);
    CHECK(tiny.status().ring_level_bytes == tooMuch.len);
    const bool wrapReuseOk = tiny.status().nal_accepted == 2;
    CHECK(wrapReuseOk);

    CopyRingBitstreamProducer seqRing(128);
    CHECK(seqRing.begin(50) == ControlResult::Ok);
    NalView badSeq{50, 7, 5, idr.data(), idr.size()};
    CHECK(seqRing.pushNal(badSeq) == PushResult::Desync);
    CHECK(seqRing.status().desync_count == 1);
    CHECK(seqRing.status().last_bad_seq == 7);
    const auto seqStatus = seqRing.status();

    // Seek/flush must not splice a partial NAL from the old session into the new one.
    CopyRingBitstreamProducer seekRing(4096);
    NalDispatcher seekDispatch(seekRing, cfg);
    AnnexBFramer seekFramer;
    CHECK(seekDispatch.begin(55) == ControlResult::Ok);
    const uint8_t partial[] = {0, 0, 0, 1, 0x65, 0xaa, 0xbb};
    CHECK(seekFramer.push(partial, sizeof(partial), [&](const uint8_t* p, size_t n) {
        CHECK(seekDispatch.handleNal(p, n) == PushResult::Ok);
    }));
    CHECK(seekRing.status().nal_accepted == 0);
    seekFramer.reset();
    CHECK(seekDispatch.flushForSeek(56) == ControlResult::Ok);
    CHECK(seekRing.status().session_id == 56);
    CHECK(seekRing.status().nal_accepted == 0);
    CHECK(seekDispatch.handleNal(sps.data(), sps.size()) == PushResult::Ok);
    CHECK(seekDispatch.handleNal(pps.data(), pps.size()) == PushResult::Ok);
    CHECK(seekDispatch.handleNal(idr.data(), idr.size()) == PushResult::Ok);
    CHECK(seekRing.status().nal_accepted == 3);
    CHECK(seekDispatch.end() == ControlResult::Ok);

    const auto fixture = readFile("tests/fixtures/p3_multinal/wcap_residual14_idr_plus_p.264");
    CHECK(fixture.size() == 9060);
    CopyRingBitstreamProducer fixtureRing(64 * 1024);
    NalDispatcher fixtureDispatch(fixtureRing, cfg);
    AnnexBFramer framer;
    CHECK(fixtureDispatch.begin(60) == ControlResult::Ok);
    size_t fixtureNals = 0;
    size_t fixtureVcl = 0;
    size_t fixtureIdr = 0;
    for (size_t off = 0; off < fixture.size();) {
        const size_t chunk = std::min<size_t>((off % 17) + 1, fixture.size() - off);
        CHECK(framer.push(fixture.data() + off, chunk, [&](const uint8_t* p, size_t n) {
            const uint8_t type = annexBNalType(p, n);
            ++fixtureNals;
            if (type == 1 || type == 5)
                ++fixtureVcl;
            if (type == 5)
                ++fixtureIdr;
            CHECK(fixtureDispatch.handleNal(p, n) == PushResult::Ok);
        }));
        off += chunk;
    }
    // The last NAL has no following start code; finish() must emit it cleanly.
    CHECK(fixtureNals == 4);
    CHECK(framer.finish([&](const uint8_t* p, size_t n) {
        const uint8_t type = annexBNalType(p, n);
        ++fixtureNals;
        if (type == 1 || type == 5)
            ++fixtureVcl;
        if (type == 5)
            ++fixtureIdr;
        CHECK(fixtureDispatch.handleNal(p, n) == PushResult::Ok);
    }));
    CHECK(fixtureNals == 5);
    CHECK(fixtureVcl == 2);
    CHECK(fixtureIdr == 1);
    CHECK(fixtureRing.status().bytes_accepted == fixture.size());
    CHECK(fixtureRing.status().nal_accepted == fixtureNals);
    CHECK(fixtureDispatch.end() == ControlResult::Ok);

    if (fails) {
        std::fprintf(stderr, "test_h264_bitstream_source: %d failures\n", fails);
        return 1;
    }
    std::printf("test_h264_bitstream_source: telemetry overrun=%llu underrun=%llu desync=%llu last_bad_seq=%u wrap_reuse=%d fixture_nals=%zu fixture_vcl=%zu fixture_bytes=%llu\n",
                static_cast<unsigned long long>(pressure.overrun_count),
                static_cast<unsigned long long>(pressure.underrun_count),
                static_cast<unsigned long long>(seqStatus.desync_count), seqStatus.last_bad_seq,
                wrapReuseOk ? 1 : 0, fixtureNals, fixtureVcl,
                static_cast<unsigned long long>(fixtureRing.status().bytes_accepted));
    std::printf("test_h264_bitstream_source: OK copy-on-push pause-full-noreplay full/underrun/overrun/desync\n");
    return 0;
}
