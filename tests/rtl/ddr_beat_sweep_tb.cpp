#include "Vddr_beat_sweep_tb.h"
#include "verilated.h"
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>

struct Result {
    int latency;
    uint32_t issued;
    uint32_t seen;
    bool pass;
    bool deadlocked;
};

static Result run_one(int /*latency_param_ignored_in_compiled_model*/) {
    auto top = std::make_unique<Vddr_beat_sweep_tb>();

    constexpr int64_t HALF_DDR_PS = 5556;   // 90 MHz
    constexpr int64_t HALF_SYS_PS = 25000;  // 20 MHz

    top->reset = 1;
    top->clk_ddr = 0;
    top->clk_sys = 0;

    int64_t t_ps = 0;
    int64_t next_ddr = HALF_DDR_PS;
    int64_t next_sys = HALF_SYS_PS;

    auto advance = [&]() {
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

    while (t_ps < 500000) advance();  // reset
    top->reset = 0;
    top->eval();

    constexpr int64_t TIMEOUT_PS = 5000000000LL;  // 5 ms
    while (!top->test_done && t_ps < TIMEOUT_PS) advance();

    Result r{};
    r.issued = top->ddr_beats_issued;
    r.seen = top->sys_beats_seen;
    r.pass = top->test_pass;
    r.deadlocked = top->test_deadlocked;
    return r;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);

    // We can only test the compiled-in DDR_LATENCY.
    // The Makefile/script should compile and run once per latency.
    Result r = run_one(0);

    std::cout << "BEAT_SWEEP"
              << " ddr_beats_issued=" << r.issued
              << " sys_beats_seen=" << r.seen
              << " pass=" << r.pass
              << " deadlocked=" << r.deadlocked
              << "\n";

    if (r.deadlocked) {
        std::cerr << "DEADLOCK after " << r.seen << "/" << r.issued << " beats\n";
        return 2;
    }
    if (!r.pass) {
        std::cerr << "FAIL: " << r.seen << "/" << r.issued << " beats\n";
        return 1;
    }
    return 0;
}
