#include "Vsdram_dq_turnaround_top.h"
#include "verilated.h"

#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

enum class Cmd {
    CkeLow,
    Deselect,
    Nop,
    Active,
    Read,
    Write,
    Precharge,
    AutoRefresh,
    ModeRegisterSet,
    Other,
};

struct Trace {
    uint64_t cycle;
    Cmd cmd;
    bool device_drive;
    bool ctl_drive;
    bool ready;
    uint16_t bus;
    uint16_t dout;
};

const char* name(Cmd cmd) {
    switch (cmd) {
    case Cmd::CkeLow: return "CKE_LOW";
    case Cmd::Deselect: return "DESELECT";
    case Cmd::Nop: return "NOP";
    case Cmd::Active: return "ACTIVE";
    case Cmd::Read: return "READ";
    case Cmd::Write: return "WRITE";
    case Cmd::Precharge: return "PRECHARGE";
    case Cmd::AutoRefresh: return "AUTO_REFRESH";
    case Cmd::ModeRegisterSet: return "MODE_REGISTER_SET";
    case Cmd::Other: return "OTHER";
    }
    return "OTHER";
}

Cmd decode(const Vsdram_dq_turnaround_top& top) {
    if (!top.SDRAM_CKE) return Cmd::CkeLow;
    if (top.SDRAM_nCS) return Cmd::Deselect;
    const unsigned bits = (static_cast<unsigned>(top.SDRAM_nRAS) << 2) |
                          (static_cast<unsigned>(top.SDRAM_nCAS) << 1) |
                          static_cast<unsigned>(top.SDRAM_nWE);
    switch (bits) {
    case 0b111: return Cmd::Nop;
    case 0b011: return Cmd::Active;
    case 0b101: return Cmd::Read;
    case 0b100: return Cmd::Write;
    case 0b010: return Cmd::Precharge;
    case 0b001: return Cmd::AutoRefresh;
    case 0b000: return Cmd::ModeRegisterSet;
    default: return Cmd::Other;
    }
}

void tick(Vsdram_dq_turnaround_top& top, uint64_t& cycle) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
    ++cycle;
    top.clk = 0;
    top.eval();
}

void print_trace(const std::vector<Trace>& trace) {
    std::cout << "DQ turnaround trace:\n";
    for (const auto& t : trace) {
        std::cout << "  cycle " << std::setw(5) << t.cycle
                  << ": cmd=" << std::setw(18) << std::left << name(t.cmd)
                  << std::right << " dev_drive=" << (t.device_drive ? 1 : 0)
                  << " ctl_drive=" << (t.ctl_drive ? 1 : 0)
                  << " ready=" << (t.ready ? 1 : 0)
                  << " bus=0x" << std::hex << std::setw(4) << std::setfill('0') << t.bus
                  << " dout=0x" << std::setw(4) << t.dout
                  << std::dec << std::setfill(' ') << "\n";
    }
}

bool run_until_first_read_sample(Vsdram_dq_turnaround_top& top,
                                 std::vector<Trace>& trace,
                                 uint64_t& first_read_cycle,
                                 uint64_t& first_capture_cycle,
                                 bool& capture_inside_drive,
                                 bool& contention,
                                 uint64_t max_cycles) {
    uint64_t cycle = 0;
    top.reset = 1;
    top.pll_locked = 0;
    for (int i = 0; i < 8; ++i) tick(top, cycle);
    top.reset = 0;
    top.pll_locked = 1;

    bool saw_first_read = false;
    bool prev_dev_drive = false;
    int after_read = -1;
    for (; cycle < max_cycles; ) {
        tick(top, cycle);
        const Cmd cmd = decode(top);
        const bool dev_drive = top.device_drive;
        const bool ctl_drive = top.ctl_dq_drive;
        const bool interesting = saw_first_read || cmd == Cmd::Read || cmd == Cmd::Write ||
                                 cmd == Cmd::Active || cmd == Cmd::ModeRegisterSet ||
                                 dev_drive || top.sdram_ready;

        if (dev_drive && ctl_drive) contention = true;

        if (cmd == Cmd::Read && !saw_first_read) {
            saw_first_read = true;
            first_read_cycle = cycle;
            after_read = 0;
        }
        if (saw_first_read && after_read >= 0 && after_read <= 12) {
            trace.push_back(Trace{cycle, cmd, dev_drive, ctl_drive, static_cast<bool>(top.sdram_ready),
                                  static_cast<uint16_t>(top.dq_bus), static_cast<uint16_t>(top.sdram_dout)});
            ++after_read;
        } else if (!saw_first_read && interesting && trace.size() < 20) {
            trace.push_back(Trace{cycle, cmd, dev_drive, ctl_drive, static_cast<bool>(top.sdram_ready),
                                  static_cast<uint16_t>(top.dq_bus), static_cast<uint16_t>(top.sdram_dout)});
        }

        if (saw_first_read && cycle > first_read_cycle && top.sdram_dout == 0x1357) {
            first_capture_cycle = cycle;
            capture_inside_drive = dev_drive || prev_dev_drive;
            return true;
        }
        prev_dev_drive = dev_drive;
    }
    return false;
}

bool run_floating(Vsdram_dq_turnaround_top& top, uint64_t max_cycles, bool require_saturation) {
    uint64_t cycle = 0;
    top.reset = 1;
    top.pll_locked = 0;
    for (int i = 0; i < 8; ++i) tick(top, cycle);
    top.reset = 0;
    top.pll_locked = 1;
    bool saw_first_fail = false;
    for (; cycle < max_cycles; ) {
        tick(top, cycle);
        if (top.first_fail_valid && (!saw_first_fail || !require_saturation)) {
            std::cout << "Floating-DQ first-fail telemetry at cycle " << cycle << ":\n"
                      << "  state_code=" << static_cast<unsigned>(top.memtest_state)
                      << " size_code=" << static_cast<unsigned>(top.memtest_size)
                      << " error_count=0x" << std::hex << std::setw(4) << std::setfill('0')
                      << static_cast<unsigned>(top.memtest_errors)
                      << " read_sample=0x" << std::setw(4)
                      << static_cast<unsigned>(top.memtest_read_sample)
                      << " first_fail_valid=" << std::dec << static_cast<unsigned>(top.first_fail_valid)
                      << " first_fail_addr=" << static_cast<unsigned>(top.first_fail_addr)
                      << " first_fail_expect=0x" << std::hex << std::setw(4)
                      << static_cast<unsigned>(top.first_fail_expect)
                      << std::dec << std::setfill(' ') << "\n";
            saw_first_fail = true;
            if (!require_saturation) {
                return top.memtest_size == 0 &&
                       top.memtest_read_sample == 0xffff &&
                       top.first_fail_valid &&
                       top.first_fail_addr == 0 &&
                       top.first_fail_expect == 0x1357;
            }
        }
        if (require_saturation && saw_first_fail && top.memtest_errors == 0xffff) {
            std::cout << "Floating-DQ saturated telemetry at cycle " << cycle << ":\n"
                      << "  state_code=" << static_cast<unsigned>(top.memtest_state)
                      << " size_code=" << static_cast<unsigned>(top.memtest_size)
                      << " error_count=0x" << std::hex << std::setw(4) << std::setfill('0')
                      << static_cast<unsigned>(top.memtest_errors)
                      << " read_sample=0x" << std::setw(4)
                      << static_cast<unsigned>(top.memtest_read_sample)
                      << " first_fail_valid=" << std::dec << static_cast<unsigned>(top.first_fail_valid)
                      << " first_fail_addr=" << static_cast<unsigned>(top.first_fail_addr)
                      << " first_fail_expect=0x" << std::hex << std::setw(4)
                      << static_cast<unsigned>(top.first_fail_expect)
                      << std::dec << std::setfill(' ') << "\n";
            return top.memtest_size == 0 &&
                   top.memtest_errors == 0xffff &&
                   top.memtest_read_sample == 0xffff &&
                   top.first_fail_valid &&
                   top.first_fail_addr == 0 &&
                   top.first_fail_expect == 0x1357;
        }
    }
    std::cerr << "FAIL: floating-DQ " << (require_saturation ? "saturation" : "first failure")
              << " not observed before timeout\n";
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);

    bool floating_mode = false;
    bool saturate_mode = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--floating") floating_mode = true;
        if (arg == "--floating-saturate") {
            floating_mode = true;
            saturate_mode = true;
        }
    }

    Vsdram_dq_turnaround_top top;
    top.clk = 0;
    top.reset = 1;
    top.pll_locked = 0;

    if (floating_mode) {
        const bool ok = run_floating(top, saturate_mode ? 220'000'000ULL : 12350ULL, saturate_mode);
        if (!ok) {
            std::cerr << "FAIL: floating-DQ telemetry did not match observed 0xffff addr0 signature\n";
            return 1;
        }
        if (saturate_mode) {
            std::cout << "PASS: floating DQ reproduces saturated 0xffff addr0 signature\n";
        } else {
            std::cout << "PASS: floating DQ reproduces read_sample=0xffff, first_fail_addr=0, size_code=0\n";
        }
        return 0;
    }

    std::vector<Trace> trace;
    uint64_t first_read_cycle = 0;
    uint64_t first_capture_cycle = 0;
    bool capture_inside_drive = false;
    bool contention = false;
    const bool got_capture = run_until_first_read_sample(top, trace, first_read_cycle,
                                                         first_capture_cycle, capture_inside_drive,
                                                         contention, 12350);
    print_trace(trace);
    std::cout << "Mode CAS latency decoded by model: " << static_cast<unsigned>(top.device_cas_latency)
              << " burst_len=" << static_cast<unsigned>(top.device_burst_len) << "\n";
    std::cout << "First READ command cycle: " << first_read_cycle << "\n";
    std::cout << "First controller data-capture cycle after READ: " << first_capture_cycle << "\n";
    std::cout << "Capture aligned with device drive window: " << (capture_inside_drive ? "yes" : "no") << "\n";
    std::cout << "Controller/device DQ contention observed: " << (contention ? "yes" : "no") << "\n";

    if (!got_capture) {
        std::cerr << "FAIL: no read capture observed before timeout\n";
        return 1;
    }
    if (contention) {
        std::cerr << "FAIL: controller and SDRAM model drove DQ at the same time\n";
        return 2;
    }
    if (!capture_inside_drive) {
        std::cerr << "FAIL: controller captured outside the SDRAM model drive window\n";
        return 3;
    }
    if (top.sdram_dout == 0xffff) {
        std::cerr << "FAIL: controller captured floating 0xffff despite active device model\n";
        return 4;
    }

    std::cout << "PASS: read capture aligns with the model drive window without contention\n";
    return 0;
}
