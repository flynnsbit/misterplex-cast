#include "Vddr_beat_conservation_tb.h"
#include "verilated.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vddr_beat_conservation_tb top{};

    // clk_ddr = 90 MHz (period 11.111 ns → 5.556 ns half)
    // clk_sys = 20 MHz (period 50 ns → 25 ns half)
    // Same PLL, 0 ps phase offset — rising edges align at t=0, then
    // clk_ddr ticks 4.5× faster.
    //
    // Simulate with integer picosecond resolution:
    // half_ddr = 5556 ps, half_sys = 25000 ps

    constexpr int64_t HALF_DDR_PS = 5556;   // 5.556 ns
    constexpr int64_t HALF_SYS_PS = 25000;  // 25.0 ns

    // Reset
    top.reset = 1;
    top.clk_ddr = 0;
    top.clk_sys = 0;

    // Run reset for a few cycles of both clocks
    int64_t t_ps = 0;
    int64_t next_ddr = HALF_DDR_PS;
    int64_t next_sys = HALF_SYS_PS;

    auto tick_to = [&](int64_t target) {
        while (t_ps < target) {
            int64_t step = target - t_ps;
            if (next_ddr - t_ps < step) step = next_ddr - t_ps;
            if (next_sys - t_ps < step) step = next_sys - t_ps;
            t_ps += step;
            if (t_ps == next_ddr) {
                top.clk_ddr = !top.clk_ddr;
                next_ddr += HALF_DDR_PS;
            }
            if (t_ps == next_sys) {
                top.clk_sys = !top.clk_sys;
                next_sys += HALF_SYS_PS;
            }
            top.eval();
        }
    };

    // Reset phase: run both clocks for 500 ns
    tick_to(500000);
    top.reset = 0;
    top.eval();

    // Run until test_done or timeout (500 us = enough for 100 reads)
    constexpr int64_t TIMEOUT_PS = 500000000LL; // 500 us
    while (!top.test_done && t_ps < TIMEOUT_PS) {
        // Advance 1 clk_ddr half-period at a time
        int64_t next = (next_ddr < next_sys) ? next_ddr : next_sys;
        tick_to(next);
    }

    // Read results
    uint32_t ddr_issued = top.ddr_beats_issued;
    uint32_t ddr_reads  = top.ddr_reads_sent;
    uint32_t sys_seen   = top.sys_beats_seen;
    bool pass           = top.test_pass;
    bool done           = top.test_done;

    std::cout << "BEAT_CONSERVATION ddr_reads_sent=" << ddr_reads
              << " ddr_beats_issued=" << ddr_issued
              << " sys_beats_seen=" << sys_seen
              << " dropped=" << (ddr_issued - sys_seen)
              << " test_done=" << done
              << " test_pass=" << pass
              << " sim_time_ns=" << (t_ps / 1000) << "\n";

    if (!done) {
        std::cerr << "FAIL beat conservation: test did not complete (timeout)\n";
        return 1;
    }
    if (!pass) {
        std::cerr << "FAIL beat conservation: sys_beats_seen=" << sys_seen
                  << " != ddr_beats_issued=" << ddr_issued
                  << " — DROPPED " << (ddr_issued - sys_seen) << " BEATS\n";
        return 1;
    }
    std::cout << "OK beat conservation: all " << ddr_issued << " beats seen by clk_sys consumer\n";
    return 0;
}
