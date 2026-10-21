#include "Vstream_path_ddr_ring_tb_top.h"
#include "verilated.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint32_t DATA_PHYS = 0x30100000u;
constexpr uint32_t CTRL_PHYS = 0x30140000u;
constexpr uint32_t STAT2_PHYS = 0x30140028u;
constexpr uint32_t STAT6_PHYS = 0x30140048u;
constexpr uint32_t RING_BYTES = 262144u;
constexpr uint32_t DATA_W = DATA_PHYS >> 3;
constexpr uint32_t CTRL_W = CTRL_PHYS >> 3;
constexpr uint32_t STAT2_W = STAT2_PHYS >> 3;
constexpr uint32_t STAT6_W = STAT6_PHYS >> 3;
constexpr uint32_t MAGIC_CTRL = 0x504C5842u;
constexpr uint32_t MAGIC_REC = 0x504C584Eu;
constexpr uint32_t MAGIC_ST2 = 0x504C5856u;
constexpr uint32_t MAGIC_ST6 = 0x504C5851u;
constexpr uint8_t EVENT_BEGIN = 1;
constexpr uint8_t EVENT_NAL = 2;

int failures = 0;

void expect(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << msg << "\n";
        ++failures;
    }
}

struct DdrModel {
    struct Pending {
        int delay = 0;
        uint32_t addr = 0;
    };

    std::unordered_map<uint32_t, uint64_t> mem;
    std::vector<Pending> pending;

    uint64_t load(uint32_t addr) const {
        auto it = mem.find(addr);
        return it == mem.end() ? 0 : it->second;
    }

    void storeByte(uint32_t byteOffset, uint8_t v) {
        uint32_t qaddr = DATA_W + ((byteOffset & (RING_BYTES - 1u)) >> 3);
        uint32_t lane = byteOffset & 7u;
        uint64_t word = load(qaddr);
        word &= ~(0xffull << (lane * 8u));
        word |= static_cast<uint64_t>(v) << (lane * 8u);
        mem[qaddr] = word;
    }

    void writeRing(uint32_t absoluteStart, const std::vector<uint8_t>& bytes) {
        for (size_t i = 0; i < bytes.size(); ++i)
            storeByte(absoluteStart + static_cast<uint32_t>(i), bytes[i]);
    }

    static void put32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
        v[off + 0] = static_cast<uint8_t>(x);
        v[off + 1] = static_cast<uint8_t>(x >> 8);
        v[off + 2] = static_cast<uint8_t>(x >> 16);
        v[off + 3] = static_cast<uint8_t>(x >> 24);
    }

    static void put64(std::vector<uint8_t>& v, size_t off, uint64_t x) {
        put32(v, off, static_cast<uint32_t>(x));
        put32(v, off + 4, static_cast<uint32_t>(x >> 32));
    }

    static std::vector<uint8_t> record(uint8_t event, uint64_t session, uint32_t seq,
                                       uint8_t nalType, const std::vector<uint8_t>& payload = {}) {
        std::vector<uint8_t> out(32 + payload.size(), 0);
        put32(out, 0, MAGIC_REC);
        out[4] = event;
        out[5] = nalType;
        put64(out, 8, session);
        put32(out, 16, seq);
        put32(out, 20, static_cast<uint32_t>(payload.size()));
        std::copy(payload.begin(), payload.end(), out.begin() + 32);
        return out;
    }

    uint32_t writeRecord(uint32_t absoluteStart, uint8_t event, uint64_t session,
                         uint32_t seq, uint8_t nalType,
                         const std::vector<uint8_t>& payload = {}) {
        auto r = record(event, session, seq, nalType, payload);
        writeRing(absoluteStart, r);
        return absoluteStart + static_cast<uint32_t>(r.size());
    }

    void publishCtrl(uint32_t writeCount, bool epoch) {
        mem[CTRL_W] = (static_cast<uint64_t>(epoch ? 1u : 0u) << 63) |
                      (static_cast<uint64_t>(writeCount & 0x7fffffffu) << 32) |
                      MAGIC_CTRL;
    }

    void afterPosedge(Vstream_path_ddr_ring_tb_top& dut) {
        if (dut.ddr_we)
            mem[static_cast<uint32_t>(dut.ddr_addr)] = dut.ddr_din;
        if (dut.ddr_rd)
            pending.push_back(Pending{2, static_cast<uint32_t>(dut.ddr_addr)});

        dut.ddr_model_dout_ready = 0;
        dut.ddr_model_dout = 0;
        std::vector<Pending> next;
        for (auto p : pending) {
            --p.delay;
            if (p.delay <= 0 && !dut.ddr_model_dout_ready) {
                dut.ddr_model_dout = load(p.addr);
                dut.ddr_model_dout_ready = 1;
            } else {
                next.push_back(p);
            }
        }
        pending.swap(next);
    }
};

void tick(Vstream_path_ddr_ring_tb_top& dut, DdrModel& ddr) {
    dut.clk = 0;
    dut.eval();
    dut.clk = 1;
    dut.eval();
    ddr.afterPosedge(dut);
}

void reset(Vstream_path_ddr_ring_tb_top& dut, DdrModel& ddr) {
    dut.reset = 1;
    dut.ioctl_download = 0;
    dut.ioctl_wr = 0;
    dut.ioctl_dout = 0;
    dut.enable = 1;
    dut.flush = 0;
    dut.ddr_stream_enable = 0;
    dut.ddr_busy = 0;
    dut.ddr_model_dout = 0;
    dut.ddr_model_dout_ready = 0;
    for (int i = 0; i < 6; ++i)
        tick(dut, ddr);
    dut.reset = 0;
    tick(dut, ddr);
}

void sendIoctl(Vstream_path_ddr_ring_tb_top& dut, DdrModel& ddr,
               const std::vector<uint8_t>& bytes) {
    dut.ddr_stream_enable = 0;
    dut.ioctl_download = 1;
    for (uint8_t b : bytes) {
        dut.ioctl_dout = b;
        dut.ioctl_wr = 1;
        tick(dut, ddr);
        dut.ioctl_wr = 0;
        tick(dut, ddr);
    }
    dut.ioctl_download = 0;
    for (int i = 0; i < 300; ++i)
        tick(dut, ddr);
}

bool waitFor(Vstream_path_ddr_ring_tb_top& dut, DdrModel& ddr, int cycles,
             bool (*pred)(const Vstream_path_ddr_ring_tb_top&)) {
    for (int i = 0; i < cycles; ++i) {
        tick(dut, ddr);
        if (pred(dut))
            return true;
    }
    return false;
}

bool count2(const Vstream_path_ddr_ring_tb_top& dut) { return dut.nalu_count >= 2; }
bool active(const Vstream_path_ddr_ring_tb_top& dut) { return dut.stream_ddr_active; }
bool hostAtWrapStart(const Vstream_path_ddr_ring_tb_top& dut) {
    return dut.stream_ddr_host_write == RING_BYTES - 3u;
}
bool overrunSeen(const Vstream_path_ddr_ring_tb_top& dut) {
    return dut.stream_ddr_overruns != 0;
}
bool underrunSeen(const Vstream_path_ddr_ring_tb_top& dut) {
    return dut.stream_ddr_underruns != 0;
}
bool wrapBytesDone(const Vstream_path_ddr_ring_tb_top& dut) {
    return dut.stream_ddr_bytes_out >= 13;
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

int startCodeLen(const std::vector<uint8_t>& bytes, size_t pos) {
    if (pos + 3 <= bytes.size() && bytes[pos] == 0 && bytes[pos + 1] == 0 &&
        bytes[pos + 2] == 1)
        return 3;
    if (pos + 4 <= bytes.size() && bytes[pos] == 0 && bytes[pos + 1] == 0 &&
        bytes[pos + 2] == 0 && bytes[pos + 3] == 1)
        return 4;
    return 0;
}

std::vector<std::vector<uint8_t>> splitAnnexB(const std::vector<uint8_t>& bytes) {
    std::vector<size_t> starts;
    for (size_t i = 0; i + 3 < bytes.size(); ++i) {
        const int sc = startCodeLen(bytes, i);
        if (sc) {
            starts.push_back(i);
            i += static_cast<size_t>(sc - 1);
        }
    }
    std::vector<std::vector<uint8_t>> nals;
    for (size_t i = 0; i < starts.size(); ++i) {
        const size_t end = (i + 1 < starts.size()) ? starts[i + 1] : bytes.size();
        if (end > starts[i])
            nals.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(starts[i]),
                              bytes.begin() + static_cast<std::ptrdiff_t>(end));
    }
    return nals;
}

uint8_t nalType(const std::vector<uint8_t>& nal) {
    const int sc = startCodeLen(nal, 0);
    if (!sc || nal.size() <= static_cast<size_t>(sc))
        return 0;
    return static_cast<uint8_t>(nal[static_cast<size_t>(sc)] & 0x1f);
}

void runIoctlStillWorks() {
    Vstream_path_ddr_ring_tb_top dut;
    DdrModel ddr;
    reset(dut, ddr);
    std::vector<uint8_t> annexb = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00,
        0x00, 0x00, 0x01, 0x68, 0xce, 0x06,
    };
    sendIoctl(dut, ddr, annexb);
    expect(dut.nalu_count >= 2, "ioctl F3 path no longer reports >=2 NALs");
    expect(dut.last_nal_type == 0x68, "ioctl F3 path lost PPS NAL framing");
}

void runWrapNormal() {
    Vstream_path_ddr_ring_tb_top dut;
    DdrModel ddr;
    reset(dut, ddr);
    const uint32_t start = RING_BYTES - 3u;
    std::vector<uint8_t> sps = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00};
    std::vector<uint8_t> pps = {0x00, 0x00, 0x01, 0x68, 0xce, 0x06};
    ddr.publishCtrl(start, true);
    dut.ddr_stream_enable = 1;
    expect(waitFor(dut, ddr, 600, hostAtWrapStart), "DDR ring never latched initial write count");
    uint32_t wr = start;
    wr = ddr.writeRecord(wr, EVENT_BEGIN, 0x1122334455667788ull, 0, 0);
    wr = ddr.writeRecord(wr, EVENT_NAL, 0x1122334455667788ull, 0, 0x67, sps);
    wr = ddr.writeRecord(wr, EVENT_NAL, 0x1122334455667788ull, 1, 0x68, pps);
    ddr.publishCtrl(wr, true);
    expect(waitFor(dut, ddr, 4000, count2), "DDR ring wrap did not deliver two NALs");
    expect(waitFor(dut, ddr, 1000, wrapBytesDone), "DDR ring wrap did not drain all bytes");
    expect(dut.last_nal_type == 0x68, "DDR ring wrap corrupted PPS NAL type");
    expect(dut.stream_ddr_bytes_out >= sps.size() + pps.size(),
           "DDR ring bytes_out did not advance: got " +
               std::to_string(dut.stream_ddr_bytes_out) + " expected >= " +
               std::to_string(sps.size() + pps.size()));
    expect(dut.stream_ddr_host_write == wr,
           "DDR ring host write counter mismatch");
}

void runSharedFixture(const std::string& fixturePath) {
    const auto file = readFile(fixturePath);
    const auto nals = splitAnnexB(file);
    expect(nals.size() >= 2, "shared multi-NAL fixture did not contain >=2 NALs");
    if (nals.size() < 2)
        return;

    Vstream_path_ddr_ring_tb_top dut;
    DdrModel ddr;
    reset(dut, ddr);
    ddr.publishCtrl(0, true);
    dut.ddr_stream_enable = 1;
    for (int i = 0; i < 600; ++i)
        tick(dut, ddr);

    uint32_t wr = 0;
    wr = ddr.writeRecord(wr, EVENT_BEGIN, 0x3333444455556666ull, 0, 0);
    uint32_t seq = 0;
    size_t payloadBytes = 0;
    for (const auto& nal : nals) {
        wr = ddr.writeRecord(wr, EVENT_NAL, 0x3333444455556666ull, seq++,
                             nalType(nal), nal);
        payloadBytes += nal.size();
    }
    ddr.publishCtrl(wr, true);

    for (int i = 0; i < 250000 &&
                    (dut.nalu_count < nals.size() || dut.stream_ddr_bytes_out < payloadBytes);
         ++i)
        tick(dut, ddr);
    expect(dut.nalu_count >= nals.size(),
           "DDR ring shared fixture did not deliver all NALs: got " +
               std::to_string(dut.nalu_count) + " expected >= " +
               std::to_string(nals.size()));
    expect(dut.stream_ddr_bytes_out >= payloadBytes,
           "DDR ring shared fixture did not drain payload bytes");
    std::cout << "stream_path DDR ring shared fixture raw: fixture_nals=" << nals.size()
              << " payload_bytes=" << payloadBytes
              << " nalu_count=" << dut.nalu_count
              << " bytes_out=" << dut.stream_ddr_bytes_out << "\n";
}

void runUnderrun() {
    Vstream_path_ddr_ring_tb_top dut;
    DdrModel ddr;
    reset(dut, ddr);
    std::vector<uint8_t> bytes = {0, 0, 1, 0x67, 0x11, 0x22, 0x33, 0x44};
    ddr.publishCtrl(0, true);
    dut.ddr_stream_enable = 1;
    for (int i = 0; i < 600; ++i)
        tick(dut, ddr);
    uint32_t wr = 0;
    wr = ddr.writeRecord(wr, EVENT_BEGIN, 0x1111ull, 0, 0);
    wr = ddr.writeRecord(wr, EVENT_NAL, 0x1111ull, 0, 0x67, bytes);
    ddr.publishCtrl(wr, true);
    auto consumed = [&](const Vstream_path_ddr_ring_tb_top& d) {
        return d.stream_ddr_bytes_out >= bytes.size();
    };
    for (int i = 0; i < 3000 && !consumed(dut); ++i)
        tick(dut, ddr);
    expect(consumed(dut), "underrun setup never consumed initial bytes");
    expect(waitFor(dut, ddr, 2000, underrunSeen), "DDR ring underrun telemetry stayed zero");
}

void runSeqGap() {
    Vstream_path_ddr_ring_tb_top dut;
    DdrModel ddr;
    reset(dut, ddr);
    ddr.publishCtrl(0, true);
    dut.ddr_stream_enable = 1;
    for (int i = 0; i < 600; ++i)
        tick(dut, ddr);
    std::vector<uint8_t> bytes = {0, 0, 1, 0x67, 0x55};
    uint32_t wr = 0;
    wr = ddr.writeRecord(wr, EVENT_BEGIN, 0x2222ull, 0, 0);
    wr = ddr.writeRecord(wr, EVENT_NAL, 0x2222ull, 1, 0x67, bytes);
    ddr.publishCtrl(wr, true);
    bool saw = false;
    for (int i = 0; i < 3000 && !saw; ++i) {
        tick(dut, ddr);
        const uint64_t st2 = ddr.load(STAT2_W);
        const uint64_t st6 = ddr.load(STAT6_W);
        const uint16_t desyncCount = static_cast<uint16_t>(st6 >> 48);
        saw = static_cast<uint32_t>(st2) == MAGIC_ST2 &&
              static_cast<uint32_t>(st6) == MAGIC_ST6 &&
              static_cast<uint32_t>(st2 >> 32) == 1u && desyncCount != 0;
    }
    expect(saw, "DDR ring seq-gap telemetry did not report last_bad_seq=1");
    expect(dut.nalu_count == 0, "DDR ring emitted payload after seq gap");
}

void runOverrun() {
    Vstream_path_ddr_ring_tb_top dut;
    DdrModel ddr;
    reset(dut, ddr);
    ddr.publishCtrl(0, true);
    dut.ddr_stream_enable = 1;
    for (int i = 0; i < 600; ++i)
        tick(dut, ddr);
    ddr.publishCtrl(RING_BYTES + 1u, true);
    expect(waitFor(dut, ddr, 1200, overrunSeen), "DDR ring overrun telemetry stayed zero");
}

} // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    runIoctlStillWorks();
    runWrapNormal();
    if (argc >= 2)
        runSharedFixture(argv[1]);
    runUnderrun();
    runOverrun();
    runSeqGap();
    if (failures) {
        std::cerr << "stream_path DDR ring RTL check FAILED: " << failures << " failures\n";
        return 1;
    }
    std::cout << "stream_path DDR ring RTL check PASS: ioctl_NALs>=2 wrap_NALs>=2 "
                 "seq_gap_detected underrun_red_guard overrun_red_guard ring_bytes=262144\n";
    return 0;
}
