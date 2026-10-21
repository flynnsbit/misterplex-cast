#include "Vddr_beat_conservation_real_tb.h"
#include "verilated.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);

    auto top = std::make_unique<Vddr_beat_conservation_real_tb>();

    // clk_ddr = 90 MHz (half = 5556 ps), clk_sys = 20 MHz (half = 25000 ps)
    constexpr int64_t HALF_DDR_PS = 5556;
    constexpr int64_t HALF_SYS_PS = 25000;

    top->reset = 1;
    top->clk_ddr = 0;
    top->clk_sys = 0;

    int64_t t_ps = 0;
    int64_t next_ddr = HALF_DDR_PS;
    int64_t next_sys = HALF_SYS_PS;

    auto advance = [&]() {
        // Find nearest edge
        int64_t next = next_ddr < next_sys ? next_ddr : next_sys;
        t_ps = next;
        if (t_ps == next_ddr) {
            top->clk_ddr = !top->clk_ddr;
            next_ddr += HALF_DDR_PS;
        }
        if (t_ps == next_sys) {
            top->clk_sys = !top->clk_sys;
            next_sys += HALF_SYS_PS;
        }
        top->eval();
    };

    // Reset for 500 ns = 500000 ps
    while (t_ps < 500000) advance();
    top->reset = 0;
    top->eval();

    // Run until done or timeout (2 ms = 2e9 ps)
    constexpr int64_t TIMEOUT_PS = 2000000000LL;
    while (!top->test_done && t_ps < TIMEOUT_PS) advance();

    uint32_t ddr_issued = top->ddr_beats_issued;
    uint32_t sys_seen   = top->sys_beats_seen;
    uint32_t stuck      = top->arb_rsp_left_stuck_cycles;
    bool pass           = top->test_pass;
    bool done           = top->test_done;

    std::cout << "BEAT_CONSERVATION_REAL"
              << " arbiter_on=clk_ddr"
              << " ddr_beats_issued=" << ddr_issued
              << " sys_beats_seen=" << sys_seen
              << " dropped=" << (int)(ddr_issued - sys_seen)
              << " arb_stuck_cycles=" << stuck
              << " test_done=" << done
              << " test_pass=" << pass
              << " sim_time_ns=" << (t_ps / 1000) << "\n";

    if (!done) {
        std::cerr << "FAIL: test did not complete (timeout at " << (t_ps/1000) << " ns)\n";
        return 1;
    }
    if (!pass) {
        std::cerr << "FAIL: sys_beats_seen=" << sys_seen
                  << " != expected " << 50
                  << " — DROPPED " << (int)(ddr_issued > sys_seen ? ddr_issued - sys_seen : 0)
                  << " BEATS, arb_stuck=" << stuck << "\n";
        return 1;
    }
    std::cout << "OK: all beats conserved\n";
    return 0;
}
