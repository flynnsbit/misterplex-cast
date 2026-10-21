// WEDGE-P1 W-* unit rows (item 4 N5; prereg r3 / phase1-plan section 6).
// This commit lands the W-NOLOCK row: serviceWedgeRead() while Main is busy on SPI
// (spiDepth > 0) must defer -- exactly one "WEDGE deferred site=... depth=N" line,
// zero SPI ops, an eager (< 10 ms) return, wedgePending_ retained and wedgeBusy_
// released (so a second call depth-defers again instead of busy-deferring).
// The FpgaSpi unit is never open(): any stray SPI op would surface as
// "readWedgePage: not open" in lastError() or a "WEDGE site=... rd=..." page log line;
// both are asserted absent. W-OFFROW: no-wedgePage rows
// (0x480058BC) and null rows log only "WEDGE skipped row" lines: zero SPI ops, no
// bit-119 write, wedgePending_ never set. W-OFFROW, W-TRIG, W-SVC, W-ONCE, W-XTHREAD and
// W-NOBMTX have landed on this line: all seven prereg r3 W-* rows.
// C549 adds W-REARM (cold start / core return) and W-STALE (closed-gate
// latch must not survive a non-wedge open). Mutants MUT_WEDGE_NO_REARM
// and MUT_WEDGE_STALE_LATCH restore those bugs.
#include "fpga_spi.hpp"
#include "libmisterplex/fpga_build_ids.hpp"
#include "libmisterplex/ddr_bitstream_ring.hpp"
#include "libmisterplex/core_gate.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace misterplex {
int& mpxTestSpiDepth();  // fpga_spi.cpp seam, compiled in for this binary only.
int& mpxTestWedgeReadAttempts();  // fpga_spi.cpp seam: count of gate-passed attempts
std::vector<std::chrono::steady_clock::time_point> mpxTestWedgeReadTimes();  // fpga_spi.cpp seam: per-attempt timestamps
std::recursive_mutex& mpxTestWedgeSpiMutex();  // fpga_spi.cpp seam: the SPI exclusive mutex
std::recursive_mutex& mpxTestBitstreamMutex();  // fpga_spi.cpp seam: the ring mutex (W-NOBMTX)
struct WedgeReadTest {
    static bool resetRing(FpgaSpi& s, int timeoutMs) { return s.resetBitstreamRing(timeoutMs); }
    static bool pending(const FpgaSpi& s) { return s.wedgePending_.load(); }
    struct Page {
        bool ok = false;
    };
    static Page readPage(FpgaSpi& s) {
        FpgaSpi::WedgePage out;
        Page p;
        p.ok = s.readWedgePage(out);
        return p;
    }
};
}  // namespace misterplex

namespace {

using namespace misterplex;

int fails = 0;
void expect(bool cond, const std::string& msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL test_wedge_read: %s\n", msg.c_str());
        ++fails;
    }
}

long count(const std::string& hay, const std::string& needle) {
    long n = 0;
    size_t p = 0;
    while ((p = hay.find(needle, p)) != std::string::npos) {
        ++n;
        p += needle.size();
    }
    return n;
}

// Captures stderr to a temp file for one window (stderr is the daemon's log channel).
class StderrCap {
public:
    void begin() {
        char tmpl[] = "/tmp/mpx_wedge_read_XXXXXX";
        fd_ = ::mkstemp(tmpl);
        if (fd_ < 0) {
            std::fprintf(stderr, "FAIL test_wedge_read: mkstemp\n");
            std::exit(3);
        }
        path_ = tmpl;
        saved_ = ::dup(2);
        ::dup2(fd_, 2);
        fflush(stderr);
    }
    std::string end() {
        fflush(stderr);
        if (saved_ >= 0) {
            ::dup2(saved_, 2);
            ::close(saved_);
            saved_ = -1;
        }
        std::ostringstream ss;
        {
            std::ifstream in(path_, std::ios::binary);
            ss << in.rdbuf();
        }
        ::close(fd_);
        fd_ = -1;
        ::unlink(path_.c_str());
        return ss.str();
    }

private:
    int fd_ = -1;
    int saved_ = -1;
    std::string path_;
};

bool makeFakeMem() {
    const int fd = ::open(MPX_DEVMEM_PATH, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    const bool ok = ::ftruncate(
        fd, ddr_bitstream_ring::kDataPhys + ddr_bitstream_ring::kRingBytes + 0x1000) == 0;
    ::close(fd);
    return ok;
}

}  // namespace

int main() {
    const misterplex::FpgaBuildInfo* wedge = misterplex::recognisedFpgaBuild(0x48FF65F2u);
    expect(wedge != nullptr && wedge->wedgePage, "row 0x48FF65F2 exists and carries wedgePage");
    if (!wedge || !wedge->wedgePage)
        return 2;
    expect(mpxTestWedgeReadAttempts() == 0, "attempt counter starts at 0");

    FpgaSpi spi;  // never open(): zero SPI by construction.
    spi.setNegotiatedBuild(wedge);
    expect(mpxTestSpiDepth() == 0, "spiDepth gauge starts at 0");

    // (1) Arm: noteWedgeTrigger stores wedgePending_ and logs exactly one trigger line.
    StderrCap cap;
    cap.begin();
    spi.noteWedgeTrigger("W-NOLOCK");
    const std::string logArm = cap.end();
    expect(count(logArm, "WEDGE trigger site=W-NOLOCK reason=epoch-ack-timeout") == 1,
           "trigger logged exactly once");
    expect(count(logArm, "WEDGE skipped row") == 0, "row present: trigger not skipped");
    expect(count(logArm, "WEDGE deferred") == 0, "trigger does not defer");

    // (2) W-NOLOCK: Main busy on SPI -> defer. One depth-defer line, zero SPI ops,
    // eager return, no error, pending untouched.
    mpxTestSpiDepth() = 1;
    cap.begin();
    const auto t0 = std::chrono::steady_clock::now();
    spi.serviceWedgeRead("W-NOLOCK", true);
    const double ms = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count() /
                      1000.0;
    const std::string logDefer = cap.end();
    mpxTestSpiDepth() = 0;
    expect(count(logDefer, "WEDGE deferred site=W-NOLOCK depth=1") == 1,
           "exactly one depth-defer line");
    expect(count(logDefer, "WEDGE deferred busy") == 0, "not the busy-defer line");
    expect(count(logDefer, "WEDGE skipped row") == 0, "row check passed");
    expect(count(logDefer, "WEDGE site=") == 0, "no wedge page read (zero SPI ops)");
    expect(spi.lastError().find("readWedgePage") == std::string::npos,
           "no SPI op attempted on the defer path");
    expect(ms < 10.0,
           "defer returns eagerly, < 10 ms (was " + std::to_string(ms) + " ms)");

    // (3) Pending retained + busy released: the second call also depth-defers
    // (reaches the depth check, so wedgePending_ stayed) and it is the depth line,
    // not the busy line (wedgeBusy_ was cleared by the prior defer's RAII).
    mpxTestSpiDepth() = 1;
    cap.begin();
    spi.serviceWedgeRead("W-NOLOCK", true);
    const std::string logDefer2 = cap.end();
    mpxTestSpiDepth() = 0;
    expect(count(logDefer2, "WEDGE deferred site=W-NOLOCK depth=1") == 1,
           "pending retained: second call reaches the depth defer");
    expect(count(logDefer2, "WEDGE deferred busy") == 0, "wedgeBusy_ released after defer");

    // (4) W-OFFROW: a stimulus row without wedgePage (0x480058BC) and no negotiated row
    // at all. noteWedgeTrigger and serviceWedgeRead must log only "WEDGE skipped row"
    // lines: zero SPI ops, no bit-119 write (lastError stays empty), and the trigger
    // never sets wedgePending_ -- so the requirePending call is silent.
    const auto* off = misterplex::recognisedFpgaBuild(0x480058BCu);
    expect(off && !off->wedgePage, "row 0x480058BC exists without wedgePage");

    FpgaSpi a;  // never open(): zero SPI by construction.
    a.setNegotiatedBuild(off);
    const int beforeA = mpxTestWedgeReadAttempts();
    cap.begin();
    a.noteWedgeTrigger("W-OFFROW");
    a.serviceWedgeRead("W-OFFROW");
    a.serviceWedgeRead("W-OFFROW", true);
    const std::string logOffA = cap.end();
    expect(count(logOffA, "WEDGE skipped row") == 2,
           "row 480058BC: exactly two skip lines (trigger + unconditional service); "
           "the requirePending call is silent, pending was never set");
    expect(count(logOffA, "WEDGE skipped row site=W-OFFROW") == 2,
           "row 480058BC: both skip lines carry the site");
    expect(count(logOffA, "WEDGE trigger") == 0, "row 480058BC: trigger did not fire");
    expect(count(logOffA, "WEDGE deferred") == 0, "row 480058BC: nothing deferred");
    expect(count(logOffA, "WEDGE site=") == 0, "row 480058BC: no wedge page read");
    expect(mpxTestWedgeReadAttempts() == beforeA, "row 480058BC: zero SPI ops");
    expect(a.lastError().empty(), "row 480058BC: no bit-119 write attempted");

    FpgaSpi b;  // never open(), no negotiated row.
    const int beforeB = mpxTestWedgeReadAttempts();
    cap.begin();
    b.noteWedgeTrigger("W-OFFROW");
    b.serviceWedgeRead("W-OFFROW");
    b.serviceWedgeRead("W-OFFROW", true);
    const std::string logOffB = cap.end();
    expect(count(logOffB, "WEDGE skipped row") == 2,
           "no row: exactly two skip lines (trigger + unconditional service); "
           "the requirePending call is silent, pending was never set");
    expect(count(logOffB, "WEDGE skipped row site=W-OFFROW") == 2,
           "no row: both skip lines carry the site");
    expect(count(logOffB, "WEDGE trigger") == 0, "no row: trigger did not fire");
    expect(count(logOffB, "WEDGE deferred") == 0, "no row: nothing deferred");
    expect(count(logOffB, "WEDGE site=") == 0, "no row: no wedge page read");
    expect(mpxTestWedgeReadAttempts() == beforeB, "no row: zero SPI ops");
    expect(b.lastError().empty(), "no row: no bit-119 write attempted");

    // (5) W-TRIG: the resetBitstreamRing timeout path. The zero-filled fake
    // /dev/mem never carries the status magic words, so the poll loop always
    // exhausts the timeout and falls back to noteWedgeTrigger("H-a"): on the
    // off row (u) that is only a "WEDGE skipped row" line; on the wedge row
    // (t) exactly one trigger line, settings aside: identical setErr text,
    // the same ~100 ms elapsed, wedgePending_ set, and no extra SPI ops.
    expect(makeFakeMem(), "fake /dev/mem created");

    FpgaSpi u;  // reuses the section (4) off row: timed-out trigger is skipped.
    u.setNegotiatedBuild(off);
    cap.begin();
    const auto t0b = std::chrono::steady_clock::now();
    const bool okBase = WedgeReadTest::resetRing(u, 100);
    const double msBase = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - t0b)
                              .count() /
                          1000.0;
    const std::string logBaseTrig = cap.end();
    expect(!okBase, "base: resetBitstreamRing returns false on timeout");
    expect(u.lastError() == "resetBitstreamRing: no fresh FPGA reset-epoch acknowledgement",
           "base: setErr text on the timeout path (was: " + u.lastError() + ")");
    expect(count(logBaseTrig, "WEDGE skipped row site=H-a") == 1,
           "base: exactly one skipped-row line on the off row");
    expect(count(logBaseTrig, "WEDGE trigger") == 0, "base: no trigger line on the off row");
    expect(msBase >= 100.0 && msBase <= 105.0,
           "base: elapsed == base timeout +-5 ms (was " + std::to_string(msBase) + " ms)");

    FpgaSpi t;  // wedge row; the next row reuses t.
    t.setNegotiatedBuild(wedge);
    const int beforeT = mpxTestWedgeReadAttempts();
    cap.begin();
    const auto t0w = std::chrono::steady_clock::now();
    const bool okWedge = WedgeReadTest::resetRing(t, 100);
    const double msTrig = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - t0w)
                              .count() /
                          1000.0;
    const std::string logTrig = cap.end();
    expect(!okWedge, "wedge: resetBitstreamRing returns false on timeout");
    expect(t.lastError() == u.lastError(), "wedge: same setErr text as the base path");
    expect(count(logTrig, "WEDGE trigger site=H-a reason=epoch-ack-timeout") == 1,
           "wedge: exactly one trigger line");
    expect(count(logTrig, "WEDGE trigger") == 1, "wedge: trigger logged exactly once");
    expect(count(logTrig, "WEDGE site=") == 0, "wedge: no wedge page read");
    expect(count(logTrig, "WEDGE deferred") == 0, "wedge: nothing deferred");
    expect(mpxTestWedgeReadAttempts() == beforeT, "wedge: zero extra SPI ops vs base");
    expect(msTrig >= 100.0 && msTrig <= 105.0,
           "wedge: elapsed == base timeout +-5 ms (was " + std::to_string(msTrig) + " ms)");
    expect(WedgeReadTest::pending(t), "wedge: wedgePending_ set by the trigger");

    // (6) W-SVC: the service row, depth 0, no mutex held. The H-a trigger left
    // wedgePending_ set, so the requirePending call proceeds: both page reads pass
    // the row gate, bump the attempt counter, and stop at setErr("readWedgePage:
    // not open") BEFORE any setStatusBit(119, ...) -- bit 119 is never set on this
    // path (stays 0). Both VOID-class page lines are still logged via logWedgePage
    // (rd=1/2 then rd=2/2); the two reads must be >= 1000 ms apart (RED mutant
    // m-nosleep reads back to back and fails the gap check).
    expect(WedgeReadTest::pending(t), "W-SVC: pending set by W-TRIG");
    const int beforeS = mpxTestWedgeReadAttempts();
    const size_t timesS = mpxTestWedgeReadTimes().size();
    cap.begin();
    const auto t0s = std::chrono::steady_clock::now();
    t.serviceWedgeRead("W-SVC", true);
    const double msSvc = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - t0s)
                             .count() /
                         1000.0;
    const std::string logSvc = cap.end();
    expect(count(logSvc, "WEDGE site=W-SVC ") == 2, "W-SVC: two page lines logged");
    expect(count(logSvc, " rd=1/2 ") == 1, "W-SVC: exactly one rd=1/2 line");
    expect(count(logSvc, " rd=2/2 ") == 1, "W-SVC: exactly one rd=2/2 line");
    {
        const size_t p1 = logSvc.find(" rd=1/2 ");
        const size_t p2 = logSvc.find(" rd=2/2 ");
        expect(p1 != std::string::npos && p2 != std::string::npos && p1 < p2,
               "W-SVC: rd=1/2 precedes rd=2/2");
    }
    expect(count(logSvc, "WEDGE deferred") == 0, "W-SVC: depth 0, nothing deferred");
    expect(mpxTestWedgeReadAttempts() == beforeS + 2, "W-SVC: two gate-passed attempts");
    const auto ts = mpxTestWedgeReadTimes();
    expect(ts.size() == timesS + 2, "W-SVC: two attempt timestamps recorded");
    if (ts.size() == timesS + 2) {
        const double gapMs = std::chrono::duration_cast<std::chrono::microseconds>(
                                 ts[timesS + 1] - ts[timesS])
                                 .count() /
                             1000.0;
        expect(gapMs >= 1000.0,
               "W-SVC: reads >= 1000 ms apart (was " + std::to_string(gapMs) + " ms)");
    }
    expect(msSvc >= 1000.0 && msSvc < 1500.0,
           "W-SVC: service elapsed >= 1000 ms and < 1500 ms (was " + std::to_string(msSvc) +
           " ms)");
    expect(!WedgeReadTest::pending(t), "W-SVC: wedgePending_ cleared by the service");
    expect(t.lastError().find("readWedgePage: not open") != std::string::npos,
           "W-SVC: each read stopped before setStatusBit, bit 119 never set (stays 0)");
    cap.begin();
    const int beforeS2 = mpxTestWedgeReadAttempts();
    t.serviceWedgeRead("W-SVC", true);
    const std::string logSvc2 = cap.end();
    expect(logSvc2.empty(), "W-SVC: pending cleared, requirePending call is silent");
    expect(mpxTestWedgeReadAttempts() == beforeS2,
           "W-SVC: silent call made no SPI attempts");

    // (7) W-ONCE: at most one read pair in flight. Two triggers arm wedgePending_
    // (two trigger lines). Service A takes wedgeBusy_, does read 1, then sleeps
    // ~1 s with the busy flag held. During that sleep wedgePending_ is still set,
    // so service B (requirePending) passes the pending check and loses the busy
    // CAS: exactly one read pair (A's), exactly one "WEDGE deferred busy
    // site=W-ONCE-B" line, and wedgePending_ cleared exactly once. The RED
    // mutant m-noCAS (no wedgeBusy_ compare-exchange) would let B take its own
    // pair (two pairs, zero busy defers) and fail the checks below.
    FpgaSpi o;
    o.setNegotiatedBuild(wedge);
    cap.begin();
    o.noteWedgeTrigger("W-ONCE");
    o.noteWedgeTrigger("W-ONCE");
    const std::string logOnceTrig = cap.end();
    expect(count(logOnceTrig, "WEDGE trigger site=W-ONCE reason=epoch-ack-timeout") == 2,
           "W-ONCE: exactly two trigger lines for two triggers");
    expect(WedgeReadTest::pending(o), "W-ONCE: wedgePending_ set by the triggers");

    const size_t timesO = mpxTestWedgeReadTimes().size();
    cap.begin();
    std::thread svcA([&o] { o.serviceWedgeRead("W-ONCE-A", true); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    bool aStarted = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (mpxTestWedgeReadTimes().size() >= timesO + 1) {
            aStarted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    o.serviceWedgeRead("W-ONCE-B", true);
    svcA.join();
    const std::string logOnce = cap.end();
    expect(aStarted, "W-ONCE: service A started its read pair");
    expect(count(logOnce, "WEDGE site=") == 2,
           "W-ONCE: exactly one read pair (two page lines total)");
    expect(count(logOnce, "WEDGE site=W-ONCE-A ") == 2,
           "W-ONCE: service A's pair logged (rd=1/2, rd=2/2)");
    expect(count(logOnce, "WEDGE site=W-ONCE-B ") == 0, "W-ONCE: service B read no pages");
    expect(count(logOnce, "WEDGE deferred busy site=W-ONCE-B") == 1,
           "W-ONCE: service B deferred on the busy CAS");
    expect(count(logOnce, "WEDGE deferred busy") == 1, "W-ONCE: exactly one busy-defer line");
    expect(mpxTestWedgeReadTimes().size() == timesO + 2,
           "W-ONCE: exactly two read attempts in total");
    expect(!WedgeReadTest::pending(o), "W-ONCE: pending cleared once");

    // (8) W-XTHREAD: rule n1 lock-then-check. Thread B holds the SPI exclusive
    // (spiMutex locked, depth 1, exactly like an SpiExclusive frame) while A
    // (main) calls the service at its own depth 0. A must block on spiMutex until
    // B releases, then read its pair: zero "WEDGE deferred" lines (the RED mutant
    // m-racy reads depth without the lock, sees B's depth 1 and defers wrongly),
    // two page lines, A's first read timestamp >= B's release time, and A's
    // elapsed >= 1250 ms (~300 ms blocked on B, then the 1 s inter-read gap).
    FpgaSpi x;  // never open(): zero SPI by construction.
    x.setNegotiatedBuild(wedge);
    cap.begin();
    x.noteWedgeTrigger("W-XTHREAD");
    const std::string logXTrig = cap.end();
    expect(count(logXTrig, "WEDGE trigger site=W-XTHREAD reason=epoch-ack-timeout") == 1,
           "W-XTHREAD: trigger logged exactly once");
    expect(WedgeReadTest::pending(x), "W-XTHREAD: wedgePending_ set by the trigger");

    std::atomic<bool> bHeld{false};
    std::chrono::steady_clock::time_point bReleasedAt{};
    std::thread holderB([&] {
        mpxTestWedgeSpiMutex().lock();
        ++mpxTestSpiDepth();
        bHeld.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        bReleasedAt = std::chrono::steady_clock::now();
        --mpxTestSpiDepth();
        mpxTestWedgeSpiMutex().unlock();
    });
    const auto deadlineX = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!bHeld.load() && std::chrono::steady_clock::now() < deadlineX)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    expect(bHeld.load(), "W-XTHREAD: B is inside its SPI exclusive frame (depth 1)");

    const size_t timesX = mpxTestWedgeReadTimes().size();
    cap.begin();
    const auto t0x = std::chrono::steady_clock::now();
    x.serviceWedgeRead("W-XTHREAD", true);
    const double msX = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - t0x)
                           .count() /
                       1000.0;
    const std::string logX = cap.end();
    holderB.join();
    expect(count(logX, "WEDGE deferred") == 0, "W-XTHREAD: A did not defer on B's depth");
    expect(count(logX, "WEDGE site=W-XTHREAD ") == 2, "W-XTHREAD: A read after B released");
    {
        const auto tsX = mpxTestWedgeReadTimes();
        expect(tsX.size() == timesX + 2, "W-XTHREAD: A's two read attempts recorded");
        if (tsX.size() == timesX + 2)
            expect(tsX[timesX] >= bReleasedAt, "W-XTHREAD: no read started while B held");
    }
    expect(msX >= 1250.0,
           "W-XTHREAD: blocked ~300 ms on B, then the 1 s gap (was " + std::to_string(msX) +
           " ms)");
    expect(!WedgeReadTest::pending(x), "W-XTHREAD: pending cleared by the service");
    expect(mpxTestSpiDepth() == 0, "W-XTHREAD: depth back to 0 after B released");

    // (9) W-NOBMTX: the service is called while bitstreamMutex (the probed ring
    // mutex) is held, either by this thread or by another: the product (phase A)
    // defers in both cases -- exactly one "WEDGE deferred bitstream-busy
    // site=W-NOBMTX" line, zero page reads, no attempt timestamp recorded, an
    // eager (< 10 ms) return, and wedgePending_ retained. Case 3 (free) proves
    // the defer was not permanent: the pending read then runs its pair.
    FpgaSpi n;  // never open(): zero SPI by construction.
    n.setNegotiatedBuild(wedge);
    cap.begin();
    n.noteWedgeTrigger("W-NOBMTX");
    const std::string logNTrig = cap.end();
    expect(count(logNTrig, "WEDGE trigger site=W-NOBMTX reason=epoch-ack-timeout") == 1,
           "W-NOBMTX: trigger logged exactly once");
    expect(WedgeReadTest::pending(n), "W-NOBMTX: wedgePending_ set by the trigger");

    // Case 1: this thread holds bitstreamMutex (the probe is recursive).
    mpxTestBitstreamMutex().lock();
    const size_t times1 = mpxTestWedgeReadTimes().size();
    cap.begin();
    const auto t0n1 = std::chrono::steady_clock::now();
    n.serviceWedgeRead("W-NOBMTX", true);
    const double msN1 = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0n1)
                            .count() /
                        1000.0;
    const std::string logN1 = cap.end();
    mpxTestBitstreamMutex().unlock();
    expect(count(logN1, "WEDGE deferred bitstream-busy site=W-NOBMTX") == 1,
           "W-NOBMTX same thread: exactly one bitstream-busy defer line");
    expect(count(logN1, "WEDGE site=") == 0,
           "W-NOBMTX same thread: no wedge page read");
    expect(mpxTestWedgeReadTimes().size() == times1,
           "W-NOBMTX same thread: no read attempt recorded");
    expect(msN1 < 10.0,
           "W-NOBMTX same thread: defer returns eagerly, < 10 ms (was " + std::to_string(msN1) +
           " ms)");
    expect(WedgeReadTest::pending(n), "W-NOBMTX same thread: wedgePending_ retained");

    // Case 2: another thread holds bitstreamMutex.
    std::atomic<bool> bmHeld{false};
    std::atomic<bool> bmRelease{false};
    std::thread bmHolder([&] {
        mpxTestBitstreamMutex().lock();
        bmHeld = true;
        while (!bmRelease)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        mpxTestBitstreamMutex().unlock();
    });
    const auto deadlineBm = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!bmHeld.load() && std::chrono::steady_clock::now() < deadlineBm)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    expect(bmHeld.load(), "W-NOBMTX other thread: holder thread is inside the ring mutex");

    const size_t times2 = mpxTestWedgeReadTimes().size();
    cap.begin();
    const auto t0n2 = std::chrono::steady_clock::now();
    n.serviceWedgeRead("W-NOBMTX", true);
    const double msN2 = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0n2)
                            .count() /
                        1000.0;
    const std::string logN2 = cap.end();
    expect(count(logN2, "WEDGE deferred bitstream-busy site=W-NOBMTX") == 1,
           "W-NOBMTX other thread: exactly one bitstream-busy defer line");
    expect(count(logN2, "WEDGE site=") == 0,
           "W-NOBMTX other thread: no wedge page read");
    expect(mpxTestWedgeReadTimes().size() == times2,
           "W-NOBMTX other thread: no read attempt recorded");
    expect(msN2 < 10.0,
           "W-NOBMTX other thread: defer returns eagerly, < 10 ms (was " + std::to_string(msN2) +
           " ms)");
    expect(WedgeReadTest::pending(n), "W-NOBMTX other thread: wedgePending_ retained");
    bmRelease = true;
    bmHolder.join();

    // Case 3: bitstreamMutex free -- the retained defer is not permanent.
    const size_t times3 = mpxTestWedgeReadTimes().size();
    cap.begin();
    n.serviceWedgeRead("W-NOBMTX", true);
    const std::string logN3 = cap.end();
    expect(count(logN3, "WEDGE deferred") == 0, "W-NOBMTX free: nothing deferred");
    expect(count(logN3, "WEDGE site=W-NOBMTX ") == 2,
           "W-NOBMTX free: two page lines logged (rd=1/2, rd=2/2)");
    expect(mpxTestWedgeReadTimes().size() == times3 + 2,
           "W-NOBMTX free: two gate-passed attempts");
    expect(!WedgeReadTest::pending(n), "W-NOBMTX free: the defer was not permanent");

    // (10) W-REARM: cold start and core return. The gate is open on 0x48FF65F2
    // and negotiatedBuild_ is null (never set, or cleared by onCoreReturned).
    // The three row gates must arm. A non-wedge open still skips. A closed
    // managed gate with no row latches pending for the next open.
    {
        CoreGate& g = CoreGate::process();
        auto closeGate = [&] { g.resetForTest(false); };
        auto openBuild = [&](uint32_t id) {
            CoreObservation o;
            o.fabric = FabricState{true, kFpgaMgrModeUser, false};
            o.main = MainIdentity{100, 100};
            o.mainStartWallNs = 500;
            o.bootWallNs = 200;
            o.coreName = CoreFileStamp{true, "Plex", 1000};
            o.rbfName = CoreFileStamp{true, "Plex_480p", 1000};
            o.nowUs = 5000000000LL;
            (void)g.observe(o);
            o.nowUs += CoreGate::kSettleUs;
            expect(g.observe(o) == CoreGateEvent::ProbeWanted, "W-REARM: probe wanted");
            const FpgaBuildInfo* row = recognisedFpgaBuild(id);
            const char* raster = row ? row->raster : "480p";
            expect(g.probeResult(true, id, "", raster, "row", o.nowUs) == CoreGateEvent::Opened,
                   "W-REARM: gate opened");
            expect(g.open() && g.buildId() == id, "W-REARM: open on the requested build");
        };

        closeGate();
        openBuild(0x48FF65F2u);
        FpgaSpi cold;
        expect(cold.negotiatedBuild() == nullptr, "W-REARM cold: no negotiated row");
        cap.begin();
        cold.noteWedgeTrigger("W-REARM");
        const std::string logCold = cap.end();
        expect(count(logCold, "WEDGE trigger site=W-REARM reason=epoch-ack-timeout") == 1,
               "W-REARM cold: trigger armed from the open gate");
        expect(count(logCold, "WEDGE skipped row") == 0, "W-REARM cold: not skipped");
        expect(WedgeReadTest::pending(cold), "W-REARM cold: pending set");
        const int beforeCold = mpxTestWedgeReadAttempts();
        const WedgeReadTest::Page coldPage = WedgeReadTest::readPage(cold);
        expect(!coldPage.ok, "W-REARM cold: read has no map");
        expect(cold.lastError().find("readWedgePage: not open") != std::string::npos,
               "W-REARM cold: read passed the row gate");
        expect(mpxTestWedgeReadAttempts() == beforeCold + 1, "W-REARM cold: one gate-passed attempt");
        mpxTestBitstreamMutex().lock();
        cap.begin();
        const auto tRearm = std::chrono::steady_clock::now();
        cold.serviceWedgeRead("W-REARM", true);
        const double msRearm = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - tRearm)
                                   .count() /
                               1000.0;
        const std::string logColdSvc = cap.end();
        mpxTestBitstreamMutex().unlock();
        expect(count(logColdSvc, "WEDGE skipped row") == 0, "W-REARM cold: service not skipped");
        expect(count(logColdSvc, "WEDGE deferred bitstream-busy site=W-REARM") == 1,
               "W-REARM cold: service passed the row gate and deferred");
        expect(msRearm < 10.0, "W-REARM cold: service did not sleep the pair");
        expect(WedgeReadTest::pending(cold), "W-REARM cold: defer kept pending");

        cold.setNegotiatedBuild(wedge);
        cold.onCoreReturned();
        expect(cold.negotiatedBuild() == nullptr, "W-REARM return: onCoreReturned clears the row");
        expect(g.open() && g.buildId() == 0x48FF65F2u, "W-REARM return: gate still open on the wedge build");
        cap.begin();
        cold.noteWedgeTrigger("W-REARM");
        const std::string logRet = cap.end();
        expect(count(logRet, "WEDGE trigger site=W-REARM reason=epoch-ack-timeout") == 1,
               "W-REARM return: trigger re-armed after onCoreReturned");
        expect(count(logRet, "WEDGE skipped row") == 0, "W-REARM return: not skipped");
        const int beforeRet = mpxTestWedgeReadAttempts();
        const WedgeReadTest::Page retPage = WedgeReadTest::readPage(cold);
        expect(!retPage.ok, "W-REARM return: read has no map");
        expect(mpxTestWedgeReadAttempts() == beforeRet + 1,
               "W-REARM return: read passed the row gate");

        closeGate();
        openBuild(0x480058BCu);
        FpgaSpi offGate;
        const int beforeOff = mpxTestWedgeReadAttempts();
        cap.begin();
        offGate.noteWedgeTrigger("W-REARM");
        offGate.serviceWedgeRead("W-REARM");
        const std::string logOffGate = cap.end();
        expect(count(logOffGate, "WEDGE skipped row") == 2,
               "W-REARM off-row open: trigger and service still skip");
        expect(count(logOffGate, "WEDGE trigger") == 0, "W-REARM off-row open: no trigger");
        expect(!WedgeReadTest::pending(offGate), "W-REARM off-row open: pending not set");
        expect(mpxTestWedgeReadAttempts() == beforeOff, "W-REARM off-row open: no SPI attempt");

        closeGate();
        FpgaSpi latched;
        cap.begin();
        latched.noteWedgeTrigger("W-REARM");
        const std::string logLatch = cap.end();
        expect(count(logLatch, "WEDGE trigger site=W-REARM reason=epoch-ack-timeout") == 1,
               "W-REARM closed: trigger latched with no row yet");
        expect(count(logLatch, "WEDGE skipped row") == 0, "W-REARM closed: not skipped");
        expect(WedgeReadTest::pending(latched), "W-REARM closed: pending set for the next open");
        openBuild(0x48FF65F2u);
        latched.onCoreReturned();
        expect(latched.negotiatedBuild() == nullptr, "W-REARM closed: return leaves the pointer null");
        mpxTestBitstreamMutex().lock();
        cap.begin();
        latched.serviceWedgeRead("W-REARM", true);
        const std::string logLatchSvc = cap.end();
        mpxTestBitstreamMutex().unlock();
        expect(count(logLatchSvc, "WEDGE skipped row") == 0,
               "W-REARM closed: open-tick service is not skipped");
        expect(count(logLatchSvc, "WEDGE deferred bitstream-busy site=W-REARM") == 1,
               "W-REARM closed: open-tick service passed the row gate");

        // (10b) W-STALE: a latch taken while the gate is closed must not survive
        // an open on a non-wedge row. A service while the gate is still closed
        // keeps the latch.
        closeGate();
        FpgaSpi stale;
        cap.begin();
        stale.noteWedgeTrigger("W-STALE");
        const std::string logStaleLatch = cap.end();
        expect(count(logStaleLatch, "WEDGE trigger site=W-STALE reason=epoch-ack-timeout") == 1,
               "W-STALE: closed gate latches");
        expect(WedgeReadTest::pending(stale), "W-STALE: pending set while closed");
        cap.begin();
        stale.serviceWedgeRead("W-STALE", true);
        const std::string logStaleClosed = cap.end();
        expect(count(logStaleClosed, "WEDGE skipped row") == 1,
               "W-STALE: closed service skips");
        expect(WedgeReadTest::pending(stale), "W-STALE: closed gate keeps the latch");
        openBuild(0x480058BCu);
        const int beforeStale = mpxTestWedgeReadAttempts();
        cap.begin();
        stale.serviceWedgeRead("W-STALE", true);
        const std::string logStaleOpen = cap.end();
        expect(count(logStaleOpen, "WEDGE skipped row") == 1,
               "W-STALE: open non-wedge row skips");
        expect(!WedgeReadTest::pending(stale),
               "W-STALE: open non-wedge row clears the latch");
        expect(mpxTestWedgeReadAttempts() == beforeStale, "W-STALE: skip does no SPI");
        closeGate();
        openBuild(0x48FF65F2u);
        cap.begin();
        stale.serviceWedgeRead("W-STALE", true);
        const std::string logStaleNext = cap.end();
        expect(logStaleNext.empty(), "W-STALE: cleared latch does not run on the next wedge open");
        expect(!WedgeReadTest::pending(stale), "W-STALE: pending stays clear");

        // H-c: silent-gate latches; the open tick services after coreGateTickStep.
        {
            std::string path = "arm/misterplexd/media_player.cpp";
            char exe[4096];
            const ssize_t nexe = ::readlink("/proc/self/exe", exe, sizeof exe - 1);
            if (nexe > 0) {
                exe[nexe] = 0;
                const std::string bin(exe);
                const auto slash = bin.rfind('/');
                const auto build = slash == std::string::npos ? std::string::npos
                                                              : bin.rfind('/', slash - 1);
                if (build != std::string::npos)
                    path = bin.substr(0, build) + "/arm/misterplexd/media_player.cpp";
            }
            std::ifstream in(path);
            std::ostringstream ss;
            ss << in.rdbuf();
            const std::string mp = ss.str();
            expect(!mp.empty(), "W-REARM: media_player.cpp readable");
            const size_t fin = mp.find("h.finishReturn = [this] {");
            const size_t finEnd = fin == std::string::npos ? std::string::npos : mp.find("\n    };", fin);
            const std::string finBody =
                (fin == std::string::npos || finEnd == std::string::npos) ? std::string()
                                                                           : mp.substr(fin, finEnd - fin);
            expect(finBody.find("serviceWedgeRead") == std::string::npos,
                   "W-REARM: finishReturn does not service the pair under the tick locks");
            expect(mp.find("fpga_.noteWedgeTrigger(\"H-c\")") != std::string::npos,
                   "W-REARM: H-c latches while the gate is closed");
            const size_t tick =
                mp.find("(void)coreGateTickStep(CoreGate::process(), h, coreReturnPending_);");
            const std::string after = tick == std::string::npos ? std::string() : mp.substr(tick, 400);
            expect(after.find("fpga_.serviceWedgeRead(\"H-c\", true);") != std::string::npos,
                   "W-REARM: H-c service runs after coreGateTickStep returns");
        }
        g.resetForTest(true);
    }

    ::unlink(MPX_DEVMEM_PATH);

    if (fails == 0)
        std::fprintf(stdout,
                     "test_wedge_read: W-NOLOCK W-OFFROW W-TRIG W-SVC W-ONCE W-XTHREAD W-NOBMTX "
                     "W-REARM W-STALE OK\n");
    else
        std::fprintf(stdout, "test_wedge_read: %d FAIL\n", fails);
    return fails ? 1 : 0;
}