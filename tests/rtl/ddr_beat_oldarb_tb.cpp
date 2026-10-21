#include "Vddr_beat_oldarb_tb.h"
#include "verilated.h"
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto top = std::make_unique<Vddr_beat_oldarb_tb>();

    constexpr int64_t HALF_DDR_PS = 5556;
    constexpr int64_t HALF_SYS_PS = 25000;

    top->reset = 1;
    top->clk_ddr = 0;
    top->clk_sys = 0;

    int64_t t_ps = 0;
    int64_t next_ddr = HALF_DDR_PS;
    int64_t next_sys = HALF_SYS_PS;

    auto advance = [&]() {
        int64_t next = next_ddr < next_sys ? next_ddr : next_sys;
        t_ps = next;
        if (t_ps == next_ddr) { top->clk_ddr = !top->clk_ddr; next_ddr += HALF_DDR_PS; }
        if (t_ps == next_sys) { top->clk_sys = !top->clk_sys; next_sys += HALF_SYS_PS; }
        top->eval();
    };

    while (t_ps < 500000) advance();
    top->reset = 0;
    top->eval();

    constexpr int64_t TIMEOUT_PS = 5000000000LL;
    while (!top->test_done && t_ps < TIMEOUT_PS) advance();

    uint32_t issued = top->ddr_beats_issued;
    uint32_t seen   = top->sys_beats_seen;
    bool pass       = top->test_pass;
    bool done       = top->test_done;
    bool dl         = top->test_deadlocked;

    std::cout << "BEAT_OLD_ARB"
              << " ddr_beats_issued=" << issued
              << " sys_beats_seen=" << seen
              << " pass=" << pass
              << " deadlocked=" << dl
              << " sim_ns=" << (t_ps / 1000) << "\n";

    if (dl) {
        std::cerr << "DEADLOCK after " << seen << "/" << issued << " beats\n";
        return 2;
    }
    if (!pass) {
        std::cerr << "FAIL: " << seen << "/" << issued << "\n";
        return 1;
    }
    std::cout << "OK\n";
    return 0;
}
