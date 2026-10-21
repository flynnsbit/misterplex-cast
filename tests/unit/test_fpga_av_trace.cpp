#include "libmisterplex/fpga_av_trace.hpp"
#include <cassert>

#if MPX_FPGA_AV_TRACE
#include "libmisterplex/fpga_terminal.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

static std::atomic<size_t> allocations{0};
void* operator new(size_t size) {
    allocations.fetch_add(1);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void* operator new(size_t size, std::align_val_t alignment) {
    allocations.fetch_add(1);
    void* p = nullptr;
    if (!::posix_memalign(&p, static_cast<size_t>(alignment), size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { std::free(p); }

using namespace misterplex;

static FpgaAvRecord record(int64_t at, int64_t duration = 0) {
    FpgaAvRecord r;
    r.event = FpgaAvEvent::AudioDrain;
    r.flags = FpgaAvRecord::Begin | FpgaAvRecord::End;
    r.begin = at;
    r.end = at + duration;
    return r;
}

static void clocks() {
    FpgaAvPauseClock clock;
    auto now = [](int64_t& value) { value = 1000; return true; };
    assert(clock.sample(now).active == 1000);
    clock.beginUpdate();
    assert(!clock.sample(now).activeValid);
    clock.endUpdate(100, 700, true);
    const auto paused = clock.sample(now);
    assert(paused.activeValid && paused.active == 600);
    const auto changing = clock.sample([&](int64_t& value) {
        value = 1100;
        clock.beginUpdate();
        clock.endUpdate(400, 0, false);
        return true;
    });
    assert(changing.monoValid && !changing.activeValid);
    assert(clock.sample(now).active == 600);
    assert(!clock.sample([](int64_t&) { return false; }).monoValid);
    assert(!clock.sample([](int64_t&) { return false; }).activeValid);
    errno = EAGAIN;
    int64_t mono;
    assert(fpgaAvMonoUs(mono) && errno == EAGAIN);
    const auto anchor = FpgaAvAnchor::capture();
    assert(anchor.valid == 7 && anchor.monoBefore <= anchor.monoAfter && errno == EAGAIN);

    clock.beginUpdate();
    clock.endUpdate(0, 7777, true);
    std::atomic<bool> done{false};
    std::thread updater([&] {
        for (int64_t i = 0; i < 100000; ++i) {
            clock.beginUpdate();
            clock.endUpdate(i, i + 7777, true);
        }
        done.store(true);
    });
    do {
        const auto sampled = clock.sample([](int64_t& value) { value = 1000000000; return true; });
        if (sampled.activeValid) assert(sampled.active == 7777);
    } while (!done.load());
    updater.join();
}

static void boundsAndDump() {
    BasicFpgaAvTrace<3, 0, 0> trace(17, 23);
    trace.bindNonce(UINT64_MAX);
    BasicFpgaAvTrace<3, 0, 0>::Writer writer(&trace, FpgaAvLane::Audio);
    bool logged = false;
    assert(!trace.freeze() && !trace.dump([&](const std::string&) { logged = true; }));
    assert(!logged);
    const size_t before = allocations.load();
    writer.record(record(0));
    writer.record(record(1));             // sampled
    writer.record(record(2, 1000));       // slow, retained despite sampling
    writer.record(record(2000), true);   // forced
    writer.record(record(10000));        // capacity drop
    writer.record(record(10001));        // sampled, not a capacity drop
    assert(allocations.load() == before);
    writer.finish();
    assert(trace.freeze());
    assert(trace.attempted(0) == 6 && trace.skipped(0) == 2);
    assert(trace.retained(0) == 3 && trace.dropped(0) == 1);
    assert(trace.at(0, 1).end == 1002);
    BasicFpgaAvTrace<3, 0, 0>::Writer late(&trace, FpgaAvLane::Audio);
    assert(!late);
    late.record(record(20000), true);
    assert(trace.attempted(0) == 6);
    std::string output;
    assert(trace.dump([&](const std::string& s) { output += s; }));
    assert(output.find("nonce=18446744073709551615") != std::string::npos);
    assert(output.find("truncated=1") != std::string::npos);
    assert(output.find(",u,u,u,u,u,u,u,u") != std::string::npos);
    assert(output.find("serialize_emit_us=") != std::string::npos);

    BasicFpgaAvTrace<4, 0, 0> spans(1, 2);
    BasicFpgaAvTrace<4, 0, 0>::Writer spanWriter(&spans, FpgaAvLane::Audio);
    FpgaAvPauseClock clock;
    const size_t spanBefore = allocations.load();
    {
        BasicFpgaAvTrace<4, 0, 0>::Span span(spanWriter, FpgaAvEvent::AudioClock, clock);
        span.value(0, 0); // Failed MAST: the unavailable identity/count fields stay invalid.
        span.value(15, -1);
        span.uvalue(15, UINT64_MAX);
        span.acquired(); span.serviceBegin(); span.serviceEnd(); span.released(); span.force();
    }
    assert(allocations.load() == spanBefore && errno == EAGAIN);
    spanWriter.finish();
    assert(spans.freeze());
    const auto& r = spans.at(0, 0);
    assert(r.valid == ((1u << 15) | 1) && !(r.signedValues & (1u << 15)));
    assert(r.begin <= r.acquired && r.acquired <= r.serviceBegin &&
           r.serviceBegin <= r.serviceEnd && r.serviceEnd <= r.released &&
           r.released <= r.end);
    output.clear();
    spans.dump([&](const std::string& s) { output += s; });
    assert(output.find(",0,u,u,u,u,u,u,u,u,u,u,u,u,u,u,18446744073709551615") != std::string::npos);

    BasicFpgaAvTrace<3, 0, 0> transitions(1, 2);
    BasicFpgaAvTrace<3, 0, 0>::Writer changes(&transitions, FpgaAvLane::Audio);
    auto wait = record(0);
    wait.event = FpgaAvEvent::AudioWait;
    wait.valid = 1;
    wait.values[0] = unsigned(FpgaAvWait::AudibleDue);
    changes.record(wait);
    wait.begin = wait.end = 1;
    wait.values[0] = unsigned(FpgaAvWait::PcmEmpty);
    changes.record(wait);
    wait.begin = wait.end = 2;
    changes.record(wait);
    changes.finish();
    assert(transitions.freeze() && transitions.retained(0) == 2 && transitions.skipped(0) == 1);
}

static void concurrencyAndLifetime() {
    using Trace = BasicFpgaAvTrace<4096, 4, 4>;
    auto trace = std::make_shared<Trace>(1, 2);
    trace->bindNonce(3);
    Trace::Writer video(trace.get(), FpgaAvLane::Video);
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    auto produce = [trace, &ready, &go](unsigned id) {
        Trace::Writer writer(trace.get(), FpgaAvLane::Audio);
        ++ready;
        while (!go.load()) std::this_thread::yield();
        for (unsigned i = 0; i < 2000; ++i) {
            auto r = record(i);
            r.values[0] = id;
            r.values[1] = i;
            r.valid = 3;
            writer.record(r, true);
        }
    };
    std::thread a(produce, 0), b(produce, 1);
    while (ready.load() != 2) std::this_thread::yield();
    FpgaTerminalInput input;
    input.naturalEof = input.fullyDrained = input.hadPresentation = true;
    bool emitted = false, joined = false;
    finishFpgaPlayback(input, [] { return std::string("numeric-test"); }, [&] {
        assert(!trace->freeze());
        go.store(true);
        a.join(); b.join();
        joined = true;
        assert(!trace->freeze()); // The video producer still participates.
    }, [&] { assert(joined && !trace->frozen()); return true; },
    [] { return true; }, [] { return false; }, [&](const FpgaTerminalReceipt&) {
        assert(joined && !trace->frozen());
        emitted = true;
    });
    video.finish();
    assert(emitted && trace->freeze());
    assert(trace->retained(0) == 4000 && trace->dropped(0) == 0);
    bool seen[2][2000]{};
    for (size_t i = 0; i < trace->retained(0); ++i) {
        const auto& r = trace->at(0, i);
        assert(r.values[0] < 2 && r.values[1] < 2000);
        assert(!seen[r.values[0]][r.values[1]]);
        seen[r.values[0]][r.values[1]] = true;
    }
    size_t calls = 0, biggest = 0;
    trace->dump([&](const std::string& s) { ++calls; biggest = std::max(biggest, s.size()); });
    assert(calls < 32 && biggest < 65536);

    ready.store(0); go.store(false);
    auto owned = std::make_shared<Trace>(4, 5);
    std::weak_ptr<Trace> weak = owned;
    std::thread holder([keep = owned, &ready, &go] {
        Trace::Writer writer(keep.get(), FpgaAvLane::Overlay);
        ready.store(1);
        while (!go.load()) std::this_thread::yield();
        writer.record(record(0), true);
    });
    while (!ready.load()) std::this_thread::yield();
    owned.reset();
    assert(!weak.expired());
    go.store(true);
    holder.join();
    assert(weak.expired());

    for (bool stopped : {false, true}) {
        Trace retired(6, 7);
        Trace::Writer worker(&retired, FpgaAvLane::Video);
        bool terminalSent = false;
        const auto terminal = finishFpgaPlayback(input, [] { return std::string(); },
            [] {}, [] { return true; }, [] { return true; },
            [=] { return stopped; }, [&](const FpgaTerminalReceipt&) {
                terminalSent = true;
                assert(!retired.freeze());
            });
        assert(terminal == (stopped ? PlaybackTerminalState::Stopped : PlaybackTerminalState::Ended));
        worker.finish();
        assert(terminalSent && retired.freeze());
    }
}

int main() {
    clocks();
    boundsAndDump();
    concurrencyAndLifetime();
    std::printf("AV trace tests passed: record=%zu storage=%zu rows=%zu\n",
                sizeof(FpgaAvRecord), FpgaAvTrace::storageBytes(), FpgaAvTrace::maxRows());
}
#else
int main() {
    int calls = 0;
    MPX_AV_TRACE(not_a_declared_type absent; ++calls;)
    assert(calls == 0);
}
#endif
