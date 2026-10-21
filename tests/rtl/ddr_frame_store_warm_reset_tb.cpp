#include "Vddr_frame_store_warm_reset_tb.h"
#include "verilated.h"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kBasePhys = 0x30000000u;
constexpr uint32_t kBankStrideBytes = 65536u;
constexpr uint32_t kDoorbellPhys = 0x3001F000u;
constexpr uint32_t kFrameMailboxPhys = 0x3001F118u;
constexpr uint32_t kMagic = 0x504C584Bu;
constexpr uint32_t kFrameMailboxMagic = 0x504C5846u;
constexpr int kW = 80;
constexpr int kH = 48;
constexpr int kYQ = kW / 8;
constexpr int kCQ = kW / 16;
constexpr int kUQBase = (kW * kH) / 8;
constexpr int kVQBase = kUQBase + (kW * kH) / 32;
constexpr uint32_t kSeqMask = 0x1fffffffu;
constexpr int kDoorbellFormatRgb565 = 0;
constexpr int kDoorbellFormatYuv420p = 1;
constexpr uint8_t kDebugFormatError = 0xE1;

uint32_t doorbellHi(uint32_t seq, int bank, int format = kDoorbellFormatYuv420p) {
    return (static_cast<uint32_t>(bank & 1) << 31) |
           (static_cast<uint32_t>(format & 0x3) << 29) | (seq & 0x1fffffffu);
}

uint64_t pack8(uint8_t v) {
    uint64_t q = 0;
    for (int i = 0; i < 8; ++i)
        q |= static_cast<uint64_t>(v) << (i * 8);
    return q;
}

uint8_t expectedRgb(uint8_t y) {
    return y;
}

struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

class Sim {
public:
    Vddr_frame_store_warm_reset_tb top{};
    std::vector<uint64_t> mem;
    uint64_t cycle = 0;
    int busy = 0;
    int rdDelay = -1;
    uint32_t rdAddr = 0;
    int rdLeft = 0;
    int rdIndex = 0;
    int scanX = 0;
    int scanY = 0;
    bool schedHighBeforeEdge = false;
    bool schedulerArmed = false;
    bool sawSchedValid = false;
    bool sawScheduledLineRead = false;
    bool hangLineReadResponses = false;
    bool sawDroppedLineRead = false;
    bool forceDdrBusy = false;

    Sim() : mem((2 * kBankStrideBytes) / 8, 0) {
        top.clk = 0;
        top.clk_ddr = 0;
        top.reset = 0;
        top.rd_x = 0;
        top.rd_y = 0;
        top.rd_active = 0;
        top.start_req = 0;
        top.bank_sel = 0;
        top.vsync_pulse = 0;
        top.DDRAM_BUSY = 0;
        top.DDRAM_DOUT = 0;
        top.DDRAM_DOUT_READY = 0;
    }

    uint32_t offQ(uint32_t phys) const { return (phys - kBasePhys) / 8; }
    uint32_t addrOffQ(uint32_t addr) const { return addr - (kBasePhys >> 3); }

    void fillFrame(int bank, uint8_t y, uint8_t u = 128, uint8_t v = 128) {
        const uint32_t base = (bank * kBankStrideBytes) / 8;
        for (int line = 0; line < kH; ++line)
            for (int q = 0; q < kYQ; ++q)
                mem[base + line * kYQ + q] = pack8(y);
        for (int line = 0; line < kH / 2; ++line) {
            for (int q = 0; q < kCQ; ++q) {
                mem[base + kUQBase + line * kCQ + q] = pack8(u);
                mem[base + kVQBase + line * kCQ + q] = pack8(v);
            }
        }
    }

    void fillFrameChromaRows(int bank, uint8_t y, const std::vector<uint8_t>& uRows, uint8_t v = 128) {
        const uint32_t base = (bank * kBankStrideBytes) / 8;
        for (int line = 0; line < kH; ++line)
            for (int q = 0; q < kYQ; ++q)
                mem[base + line * kYQ + q] = pack8(y);
        for (int line = 0; line < kH / 2; ++line) {
            const uint8_t u = uRows.empty() ? 128 : uRows[std::min<int>(line, static_cast<int>(uRows.size()) - 1)];
            for (int q = 0; q < kCQ; ++q) {
                mem[base + kUQBase + line * kCQ + q] = pack8(u);
                mem[base + kVQBase + line * kCQ + q] = pack8(v);
            }
        }
    }

    void ringDoorbell(int bank, uint32_t seq, int format = kDoorbellFormatYuv420p) {
        const uint32_t off = offQ(kDoorbellPhys);
        mem[off] = static_cast<uint64_t>(doorbellHi(seq, bank, format)) << 32 | kMagic;
    }

    uint64_t frameMailbox() const {
        return mem[offQ(kFrameMailboxPhys)];
    }

    bool waitForFrameDebug(uint8_t debug, int maxCycles) {
        for (int i = 0; i < maxCycles; ++i) {
            const uint64_t mbox = frameMailbox();
            if (static_cast<uint32_t>(mbox) == kFrameMailboxMagic &&
                static_cast<uint8_t>((mbox >> 40) & 0xffu) == debug)
                return true;
            tick();
        }
        return false;
    }

    bool waitForFrameMailboxMagic(int maxCycles) {
        for (int i = 0; i < maxCycles; ++i) {
            if (static_cast<uint32_t>(frameMailbox()) == kFrameMailboxMagic)
                return true;
            tick();
        }
        return false;
    }

    void serviceDdrStart() {
        if (top.DDRAM_RD && busy == 0 && rdDelay < 0 && rdLeft == 0) {
            if (schedulerArmed) {
                sawScheduledLineRead = true;
                schedulerArmed = false;
            }
            if (hangLineReadResponses && top.DDRAM_ADDR != (kDoorbellPhys >> 3)) {
                sawDroppedLineRead = true;
                return;
            }
            rdAddr = top.DDRAM_ADDR;
            rdLeft = top.DDRAM_BURSTCNT;
            rdIndex = 0;
            rdDelay = 2;
            busy = rdLeft + rdDelay + 1;
        }
        if (top.DDRAM_WE && busy == 0) {
            const uint32_t off = addrOffQ(top.DDRAM_ADDR);
            if (off < mem.size())
                mem[off] = top.DDRAM_DIN;
            busy = 2;
        }
    }

    void serviceDdrDrive() {
        top.DDRAM_DOUT_READY = 0;
        if (busy > 0)
            --busy;
        top.DDRAM_BUSY = forceDdrBusy || (busy > 0);
        if (rdDelay >= 0) {
            if (rdDelay > 0) {
                --rdDelay;
            } else if (rdLeft > 0) {
                const uint32_t off = addrOffQ(rdAddr + rdIndex);
                top.DDRAM_DOUT = off < mem.size() ? mem[off] : 0;
                top.DDRAM_DOUT_READY = 1;
                ++rdIndex;
                --rdLeft;
                if (rdLeft == 0)
                    rdDelay = -1;
            }
        }
    }

    void tick() {
        top.clk = 0;
        top.clk_ddr = 0;
        top.eval();
        schedHighBeforeEdge = top.debug_sched_valid;
        if (schedHighBeforeEdge) {
            sawSchedValid = true;
            schedulerArmed = true;
        }
        serviceDdrDrive();
        top.clk = 1;
        top.clk_ddr = 1;
        top.eval();
        serviceDdrStart();
        top.clk = 0;
        top.clk_ddr = 0;
        top.eval();
        ++cycle;
        top.vsync_pulse = 0;
    }

    void videoTick() {
        top.rd_active = 1;
        top.rd_x = scanX;
        top.rd_y = scanY;
        top.vsync_pulse = (scanX == 0 && scanY == 0);
        tick();
        ++scanX;
        if (scanX == kW) {
            scanX = 0;
            ++scanY;
            if (scanY == kH)
                scanY = 0;
        }
    }

    void resetCore() {
        top.reset = 1;
        for (int i = 0; i < 8; ++i)
            tick();
        top.reset = 0;
        for (int i = 0; i < 4; ++i)
            tick();
    }

    void pulseVsync() {
        top.vsync_pulse = 1;
        tick();
    }

    bool waitCyclesNoFrame(int n) {
        for (int i = 0; i < n; ++i) {
            videoTick();
            if (top.has_frame)
                return false;
        }
        return true;
    }

    bool waitForFrame(int maxCycles) {
        const int startFrames = top.frames_done;
        return waitForFrameCount(startFrames + 1, maxCycles);
    }

    bool waitForFrameCount(int minFrames, int maxCycles) {
        for (int i = 0; i < maxCycles; ++i) {
            videoTick();
            if (top.frames_done >= minFrames)
                return true;
        }
        return false;
    }

    bool waitForFrameCountStatic(int minFrames, int maxCycles) {
        top.rd_active = 0;
        top.rd_x = 0;
        top.rd_y = 0;
        for (int i = 0; i < maxCycles; ++i) {
            if ((i % 97) == 0)
                pulseVsync();
            else
                tick();
            if (top.frames_done >= minFrames)
                return true;
        }
        return false;
    }

    uint8_t sample(int x, int y) {
        const int saveX = scanX;
        const int saveY = scanY;
        top.rd_x = x;
        top.rd_y = y;
        top.rd_active = 1;
        for (int i = 0; i < 18; ++i)
            tick();
        top.rd_active = 0;
        tick();
        scanX = saveX;
        scanY = saveY;
        return top.rd_r;
    }

    Rgb sampleRgb(int x, int y) {
        const int saveX = scanX;
        const int saveY = scanY;
        top.rd_x = x;
        top.rd_y = y;
        top.rd_active = 1;
        for (int i = 0; i < 18; ++i)
            tick();
        top.rd_active = 0;
        tick();
        scanX = saveX;
        scanY = saveY;
        return {top.rd_r, top.rd_g, top.rd_b};
    }

    bool schedulerProven() const { return sawSchedValid && sawScheduledLineRead; }
};

uint8_t stableSample(Sim& sim) {
    for (int i = 0; i < 1000; ++i)
        sim.tick();
    return sim.sample(0, 0);
}

Rgb stableSampleRgb(Sim& sim) {
    for (int i = 0; i < 1000; ++i)
        sim.tick();
    return sim.sampleRgb(0, 0);
}

Rgb stableSampleRgbAt(Sim& sim, int x, int y) {
    sim.top.rd_x = x;
    sim.top.rd_y = y;
    sim.top.rd_active = 1;
    for (int i = 0; i < 1600; ++i)
        sim.tick();
    return sim.sampleRgb(x, y);
}

void expectFreshSample(const std::string& label, Sim& sim, uint8_t want) {
    const uint8_t got = stableSample(sim);
    if (got + 1 < want || got > want + 1) {
        std::cerr << "FAIL ddr_frame_store warm-reset: " << label << " got r=" << int(got)
                  << " want≈" << int(want) << " frames=" << sim.top.frames_done
                  << " underruns=" << sim.top.underrun_count
                  << " has_frame=" << int(sim.top.has_frame)
                  << " swap_pending=" << int(sim.top.swap_pending)
                  << " debug=0x" << std::hex << int(sim.top.debug_state) << std::dec << "\n";
        std::exit(1);
    }
}

bool runFreshNoStale() {
    Sim sim;
    sim.fillFrame(1, 208);
    sim.resetCore();
    for (int i = 0; i < 3000; ++i)
        sim.tick();
    sim.ringDoorbell(1, 1);
    if (!sim.waitForFrame(50000))
        throw std::runtime_error("first fresh doorbell without stale magic did not produce a frame");
    expectFreshSample("first fresh no-stale", sim, 208);
    return sim.schedulerProven();
}

bool runInitialFrameMailboxPublish() {
    Sim sim;
    sim.resetCore();
    if (!sim.waitForFrameMailboxMagic(20000)) {
        std::cerr << "FAIL ddr_frame_store warm-reset: PLXF frame mailbox did not publish"
                  << " after reset mailbox=0x" << std::hex << sim.frameMailbox()
                  << std::dec << " cycle=" << sim.cycle << "\n";
        std::exit(1);
    }
    if (sim.cycle >= 224) {
        std::cerr << "FAIL ddr_frame_store warm-reset: initial PLXF publish waited for legacy poll slot"
                  << " cycles=" << sim.cycle << "\n";
        std::exit(1);
    }
    const uint64_t mbox = sim.frameMailbox();
    std::cout << "ddr_frame_store warm-reset raw: initial_plxf_publish"
              << " frame_mailbox_magic=0x" << std::hex << static_cast<uint32_t>(mbox)
              << " frame_debug=0x" << int((mbox >> 40) & 0xffu)
              << std::dec << " cycles=" << sim.cycle << "\n";
    return sim.schedulerProven();
}

bool runInitialFrameMailboxAbsentWhenDdrBusy() {
    Sim sim;
    sim.forceDdrBusy = true;
    sim.resetCore();
    for (int i = 0; i < 20000; ++i)
        sim.tick();
    const uint64_t mbox = sim.frameMailbox();
    if (static_cast<uint32_t>(mbox) != 0 || sim.top.has_frame) {
        std::cerr << "FAIL ddr_frame_store warm-reset: forced DDR busy should leave PLXF absent"
                  << " mailbox=0x" << std::hex << mbox
                  << " has_frame=" << std::dec << int(sim.top.has_frame) << "\n";
        std::exit(1);
    }
    std::cout << "ddr_frame_store warm-reset raw: initial_plxf_absent_when_ddr_busy"
              << " frame_mailbox_magic=0x" << std::hex << static_cast<uint32_t>(mbox)
              << " full_mailbox=0x" << mbox
              << std::dec << " has_frame=" << int(sim.top.has_frame)
              << " cycles=" << sim.cycle << "\n";
    return sim.schedulerProven();
}

bool runFrameMailboxStallsWithHungLineRead() {
    Sim sim;
    sim.fillFrame(0, 218);
    sim.resetCore();
    if (!sim.waitForFrameMailboxMagic(20000))
        throw std::runtime_error("line-read-hang setup: initial PLXF mailbox did not publish");
    sim.hangLineReadResponses = true;
    sim.ringDoorbell(0, 0x69);
    for (int i = 0; i < 100000 && !sim.sawDroppedLineRead; ++i)
        sim.videoTick();
    if (!sim.sawDroppedLineRead) {
        std::cerr << "FAIL ddr_frame_store warm-reset: line-read-hang did not reach a frame line read"
                  << " has_frame=" << int(sim.top.has_frame)
                  << " debug=0x" << std::hex << int(sim.top.debug_state)
                  << " mailbox=0x" << sim.frameMailbox() << std::dec << "\n";
        std::exit(1);
    }
    const uint64_t staleMbox = sim.frameMailbox();
    for (int i = 0; i < 20000; ++i)
        sim.videoTick();
    const uint64_t laterMbox = sim.frameMailbox();
    if (sim.top.has_frame || laterMbox != staleMbox) {
        std::cerr << "FAIL ddr_frame_store warm-reset: hung line read did not leave PLXF stale"
                  << " has_frame=" << int(sim.top.has_frame)
                  << " before=0x" << std::hex << staleMbox
                  << " after=0x" << laterMbox
                  << " live_debug=0x" << int(sim.top.debug_state) << std::dec << "\n";
        std::exit(1);
    }
    std::cout << "ddr_frame_store warm-reset raw: line_read_hang_plxf_stale"
              << " plxf=0x" << std::hex << laterMbox
              << " live_debug=0x" << int(sim.top.debug_state)
              << std::dec << " has_frame=" << int(sim.top.has_frame)
              << " cycles=" << sim.cycle << "\n";
    return sim.schedulerProven();
}

bool runChromaPlaneReadMapping() {
    Sim sim;
    sim.fillFrame(0, 128, 255, 0);
    sim.resetCore();
    for (int i = 0; i < 3000; ++i)
        sim.tick();
    sim.ringDoorbell(0, 11);
    if (!sim.waitForFrame(50000))
        throw std::runtime_error("chroma U/V read mapping: frame did not present");

    const Rgb got = stableSampleRgb(sim);
    if (!(got.r <= 8 && got.b >= 248 && got.g >= 160 && got.g <= 190)) {
        std::cerr << "FAIL ddr_frame_store warm-reset: U/V read mapping got rgb="
                  << int(got.r) << "/" << int(got.g) << "/" << int(got.b)
                  << " want blue-dominant from Y=128 U=255 V=0"
                  << " u_base_qwords=" << kUQBase << " v_base_qwords=" << kVQBase
                  << " y_stride_qwords=" << kYQ << " c_stride_qwords=" << kCQ
                  << " frames=" << sim.top.frames_done
                  << " underruns=" << sim.top.underrun_count << "\n";
        std::exit(1);
    }

    std::cout << "ddr_frame_store chroma raw: U_base_qwords=" << kUQBase
              << " V_base_qwords=" << kVQBase
              << " U_base_bytes=" << (kUQBase * 8)
              << " V_base_bytes=" << (kVQBase * 8)
              << " Y_stride_qwords=" << kYQ
              << " C_stride_qwords=" << kCQ
              << " Y_stride_bytes=" << (kYQ * 8)
              << " C_stride_bytes=" << (kCQ * 8)
              << " sample_rgb=" << int(got.r) << "/" << int(got.g) << "/" << int(got.b)
              << " frames=" << sim.top.frames_done
              << " cycles=" << sim.cycle << "\n";
    return sim.schedulerProven();
}

bool runChromaVerticalStrideMapping() {
    Sim sim;
    sim.fillFrameChromaRows(0, 128, {128, 180, 70});
    sim.resetCore();
    for (int i = 0; i < 3000; ++i)
        sim.tick();
    sim.ringDoorbell(0, 12);
    if (!sim.waitForFrame(50000))
        throw std::runtime_error("chroma vertical/stride mapping: frame did not present");

    const Rgb y0 = stableSampleRgbAt(sim, 0, 0);
    const Rgb y1 = stableSampleRgbAt(sim, 0, 1);
    const Rgb y2 = stableSampleRgbAt(sim, 0, 2);
    if (!(std::abs(int(y0.b) - 128) <= 8 && std::abs(int(y1.b) - 128) <= 8 && y2.b >= 205)) {
        std::cerr << "FAIL ddr_frame_store warm-reset: chroma vertical subsampling/stride"
                  << " y0_rgb=" << int(y0.r) << "/" << int(y0.g) << "/" << int(y0.b)
                  << " y1_rgb=" << int(y1.r) << "/" << int(y1.g) << "/" << int(y1.b)
                  << " y2_rgb=" << int(y2.r) << "/" << int(y2.g) << "/" << int(y2.b)
                  << " want y0/y1 from chroma row0 and y2 from chroma row1"
                  << " bench_y_stride_bytes=" << (kYQ * 8)
                  << " bench_c_stride_bytes=" << (kCQ * 8)
                  << " product_c_stride_bytes=312"
                  << " frames=" << sim.top.frames_done
                  << " underruns=" << sim.top.underrun_count << "\n";
        std::exit(1);
    }

    std::cout << "ddr_frame_store chroma vertical/stride raw: y0_rgb="
              << int(y0.r) << "/" << int(y0.g) << "/" << int(y0.b)
              << " y1_rgb=" << int(y1.r) << "/" << int(y1.g) << "/" << int(y1.b)
              << " y2_rgb=" << int(y2.r) << "/" << int(y2.g) << "/" << int(y2.b)
              << " bench_Y_stride_bytes=" << (kYQ * 8)
              << " bench_C_stride_bytes=" << (kCQ * 8)
              << " product_Y_stride_bytes=624 product_C_stride_bytes=312"
              << " frames=" << sim.top.frames_done << " cycles=" << sim.cycle << "\n";
    return sim.schedulerProven();
}

bool runWarmResetChanged(uint32_t staleSeq, uint32_t freshSeq, int staleBank, int freshBank,
                         uint8_t freshY, const std::string& label) {
    Sim sim;
    sim.fillFrame(0, 48);
    sim.fillFrame(1, freshY);
    sim.ringDoorbell(staleBank, staleSeq);
    sim.resetCore();

    if (!sim.waitCyclesNoFrame(25000)) {
        std::cerr << "FAIL ddr_frame_store warm-reset: accepted stale doorbell before fresh frame"
                  << " label=" << label << " cycle=" << sim.cycle
                  << " frames=" << sim.top.frames_done << "\n";
        std::exit(1);
    }

    sim.ringDoorbell(freshBank, freshSeq);
    if (!sim.waitForFrame(50000))
        throw std::runtime_error(label + ": fresh doorbell did not produce a frame");
    expectFreshSample(label, sim, freshY);

    std::cout << "ddr_frame_store warm-reset raw: " << label << " stale_seq=" << staleSeq
              << " fresh_seq=" << freshSeq << " stale_bank=" << staleBank
              << " fresh_bank=" << freshBank << " no_frame_cycles=25000 frames="
              << sim.top.frames_done << " sample_x=0 sample_y=0 sample_r=" << int(freshY)
              << " underruns=" << sim.top.underrun_count << " cycles=" << sim.cycle << "\n";
    return sim.schedulerProven();
}

bool runRejectNonYuvDoorbell() {
    {
        Sim sim;
        sim.fillFrame(0, 61);
        sim.fillFrame(1, 213);
        sim.resetCore();
        for (int i = 0; i < 3000; ++i)
            sim.tick();
        sim.ringDoorbell(0, 2, kDoorbellFormatRgb565);
        if (!sim.waitCyclesNoFrame(25000)) {
            std::cerr << "FAIL ddr_frame_store warm-reset: accepted non-YUV doorbell"
                      << " cycle=" << sim.cycle << " frames=" << sim.top.frames_done << "\n";
            std::exit(1);
        }
        if (sim.top.debug_state != kDebugFormatError) {
            std::cerr << "FAIL ddr_frame_store warm-reset: non-YUV doorbell debug=0x"
                      << std::hex << int(sim.top.debug_state) << " want=0x"
                      << int(kDebugFormatError) << std::dec << "\n";
            std::exit(1);
        }
        if (!sim.waitForFrameDebug(kDebugFormatError, 20000)) {
            std::cerr << "FAIL ddr_frame_store warm-reset: non-YUV doorbell did not publish"
                      << " PLXF frame_debug=0x" << std::hex << int(kDebugFormatError)
                      << " mailbox=0x" << sim.frameMailbox() << std::dec << "\n";
            std::exit(1);
        }
        sim.ringDoorbell(1, 2, kDoorbellFormatYuv420p);
        if (!sim.waitForFrame(50000))
            throw std::runtime_error("valid YUV doorbell did not recover after live non-YUV reject");
        expectFreshSample("live non-YUV reject then YUV accept", sim, 213);
    }

    Sim sim;
    sim.fillFrame(0, 60);
    sim.fillFrame(1, 213);
    sim.ringDoorbell(0, 4, kDoorbellFormatRgb565);
    sim.resetCore();
    if (!sim.waitCyclesNoFrame(25000)) {
        std::cerr << "FAIL ddr_frame_store warm-reset: accepted non-YUV doorbell"
                  << " cycle=" << sim.cycle << " frames=" << sim.top.frames_done << "\n";
        std::exit(1);
    }
    if (sim.top.debug_state != kDebugFormatError) {
        std::cerr << "FAIL ddr_frame_store warm-reset: non-YUV doorbell debug=0x"
                  << std::hex << int(sim.top.debug_state) << " want=0x"
                  << int(kDebugFormatError) << std::dec << "\n";
        std::exit(1);
    }
    if (!sim.waitForFrameDebug(kDebugFormatError, 20000)) {
        std::cerr << "FAIL ddr_frame_store warm-reset: non-YUV doorbell did not publish"
                  << " PLXF frame_debug=0x" << std::hex << int(kDebugFormatError)
                  << " mailbox=0x" << sim.frameMailbox() << std::dec << "\n";
        std::exit(1);
    }

    sim.ringDoorbell(1, 4, kDoorbellFormatYuv420p);
    if (!sim.waitForFrame(50000))
        throw std::runtime_error("valid YUV doorbell did not recover after non-YUV reject");
    expectFreshSample("non-YUV reject then YUV accept", sim, 213);

    std::cout << "ddr_frame_store warm-reset raw: non_yuv_reject rejected_format=0 frame_debug=0x"
              << std::hex << int(kDebugFormatError) << std::dec
              << " frame_mailbox_magic=0x" << std::hex << kFrameMailboxMagic << std::dec
              << " fresh_format=1 seq=4 stale_bank=0 fresh_bank=1 no_frame_cycles=25000"
              << " frames=" << sim.top.frames_done << " sample_r=213 underruns="
              << sim.top.underrun_count << " cycles=" << sim.cycle << "\n";
    return sim.schedulerProven();
}

bool runRunningArmRestartLower() {
    Sim sim;
    sim.fillFrame(0, 96);
    sim.resetCore();
    sim.ringDoorbell(0, 9);
    if (!sim.waitForFrame(800000))
        throw std::runtime_error("running-restart: initial frame did not present");
    expectFreshSample("running-restart initial", sim, 96);

    const int prevFrames = sim.top.frames_done;
    sim.fillFrame(1, 214);
    sim.ringDoorbell(1, 1);
    if (!sim.waitForFrameCountStatic(prevFrames + 1, 800000)) {
        std::cerr << "FAIL ddr_frame_store warm-reset: running-restart no second frame"
                  << " prev_frames=" << prevFrames << " frames=" << sim.top.frames_done
                  << " has_frame=" << int(sim.top.has_frame)
                  << " swap_pending=" << int(sim.top.swap_pending)
                  << " doorbell_ok=" << int(sim.top.doorbell_ok)
                  << " debug=0x" << std::hex << int(sim.top.debug_state) << std::dec
                  << " cycle=" << sim.cycle << "\n";
        throw std::runtime_error("running-restart: lower restarted seq did not present");
    }
    expectFreshSample("running-restart lower seq", sim, 214);

    std::cout << "ddr_frame_store warm-reset raw: running_arm_restart stale_seq=9 fresh_seq=1"
              << " stale_bank=0 fresh_bank=1 frames=" << sim.top.frames_done
              << " sample_r=214 underruns=" << sim.top.underrun_count
              << " cycles=" << sim.cycle << "\n";
    return sim.schedulerProven();
}

bool runEqualTokenFallback() {
    Sim sim;
    sim.fillFrame(0, 48);
    sim.ringDoorbell(0, 5);
    sim.resetCore();
    if (!sim.waitCyclesNoFrame(25000)) {
        std::cerr << "FAIL ddr_frame_store warm-reset: equal-token fallback fired too early"
                  << " cycle=" << sim.cycle << " frames=" << sim.top.frames_done << "\n";
        std::exit(1);
    }

    sim.fillFrame(0, 212);
    sim.ringDoorbell(0, 5);
    if (!sim.waitForFrame(800000))
        throw std::runtime_error("equal-token fallback did not recover");
    expectFreshSample("equal-token fallback", sim, 212);

    std::cout << "ddr_frame_store warm-reset raw: equal_token_fallback stale_seq=5 fresh_seq=5"
              << " stale_bank=0 fresh_bank=0 no_frame_cycles=25000 frames="
              << sim.top.frames_done << " sample_r=212 underruns=" << sim.top.underrun_count
              << " cycles=" << sim.cycle << "\n";
    return sim.schedulerProven();
}

bool runLiveValidYuvResetPrimedDoorbell() {
    Sim sim;
    sim.fillFrame(1, 218);
    sim.ringDoorbell(1, 0x68, kDoorbellFormatYuv420p);
    sim.resetCore();
    if (!sim.waitCyclesNoFrame(25000)) {
        std::cerr << "FAIL ddr_frame_store warm-reset: live valid YUV reset-primed token"
                  << " presented before fallback window cycle=" << sim.cycle
                  << " frames=" << sim.top.frames_done
                  << " debug=0x" << std::hex << int(sim.top.debug_state) << std::dec << "\n";
        std::exit(1);
    }
    if (sim.top.debug_state == kDebugFormatError) {
        std::cerr << "FAIL ddr_frame_store warm-reset: live valid YUV token raised 0x"
                  << std::hex << int(kDebugFormatError) << std::dec << "\n";
        std::exit(1);
    }
    if (!sim.waitForFrame(800000))
        throw std::runtime_error("live valid YUV reset-primed token did not recover through fallback");
    expectFreshSample("live valid YUV reset-primed token", sim, 218);

    std::cout << "ddr_frame_store warm-reset raw: live_valid_yuv_reset_primed"
              << " doorbell_hi=0xa0000068 bank=1 format=1 seq=0x68"
              << " no_frame_cycles=25000 frame_debug=0x00"
              << " frames=" << sim.top.frames_done << " sample_r=218"
              << " underruns=" << sim.top.underrun_count
              << " cycles=" << sim.cycle << "\n";
    return sim.schedulerProven();
}

bool runEqualTokenRefreshAfterAccept() {
    Sim sim;
    sim.fillFrame(0, 96);
    sim.resetCore();
    for (int i = 0; i < 3000; ++i)
        sim.tick();
    sim.ringDoorbell(0, 13);
    if (!sim.waitForFrame(50000))
        throw std::runtime_error("equal-token refresh: initial frame did not present");
    expectFreshSample("equal-token refresh initial", sim, 96);

    const int prevFrames = sim.top.frames_done;
    sim.fillFrame(0, 217);
    sim.ringDoorbell(0, 13);
    if (!sim.waitForFrameCountStatic(prevFrames + 1, 800000)) {
        std::cerr << "FAIL ddr_frame_store warm-reset: equal-token refresh after accept did not present"
                  << " prev_frames=" << prevFrames << " frames=" << sim.top.frames_done
                  << " has_frame=" << int(sim.top.has_frame)
                  << " swap_pending=" << int(sim.top.swap_pending)
                  << " doorbell_ok=" << int(sim.top.doorbell_ok)
                  << " debug=0x" << std::hex << int(sim.top.debug_state) << std::dec
                  << " cycle=" << sim.cycle << "\n";
        throw std::runtime_error("equal-token refresh after accept did not present");
    }
    expectFreshSample("equal-token refresh after accept", sim, 217);

    std::cout << "ddr_frame_store warm-reset raw: equal_token_refresh_after_accept"
              << " seq=13 bank=0 prev_frames=" << prevFrames
              << " frames=" << sim.top.frames_done << " sample_r=217 underruns="
              << sim.top.underrun_count << " cycles=" << sim.cycle << "\n";
    return sim.schedulerProven();
}

void run() {
    bool schedulerSeen = false;
    schedulerSeen |= runInitialFrameMailboxPublish();
    schedulerSeen |= runInitialFrameMailboxAbsentWhenDdrBusy();
    schedulerSeen |= runFreshNoStale();
    schedulerSeen |= runChromaPlaneReadMapping();
    schedulerSeen |= runChromaVerticalStrideMapping();
    schedulerSeen |= runWarmResetChanged(1, 2, 0, 1, expectedRgb(208), "increment");
    schedulerSeen |= runWarmResetChanged(7, 1, 0, 1, expectedRgb(209), "restart_lower_seq");
    schedulerSeen |= runWarmResetChanged(3, 3, 0, 1, expectedRgb(210), "equal_seq_changed_bank");
    schedulerSeen |= runWarmResetChanged(kSeqMask, 0, 0, 1, expectedRgb(211), "seq_wrap");
    schedulerSeen |= runRejectNonYuvDoorbell();
    schedulerSeen |= runRunningArmRestartLower();
    schedulerSeen |= runFrameMailboxStallsWithHungLineRead();
    schedulerSeen |= runEqualTokenFallback();
    schedulerSeen |= runLiveValidYuvResetPrimedDoorbell();
    schedulerSeen |= runEqualTokenRefreshAfterAccept();
    if (!schedulerSeen) {
        std::cerr << "FAIL ddr_frame_store warm-reset: refill scheduler pipeline not observed\n";
        std::exit(1);
    }
    std::cout << "OK ddr_frame_store warm-reset: stale doorbell ignored until fresh frame; refill scheduler pipelined\n";
}
} // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    try {
        run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL ddr_frame_store warm-reset: " << e.what() << "\n";
        return 1;
    }
}
