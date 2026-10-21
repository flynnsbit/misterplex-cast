#include "Vsdram_startup_top.h"
#include "verilated.h"

#include <cstdint>
#include <cstdlib>
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

struct Event {
    uint64_t cycle;
    Cmd cmd;
    bool injected;
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

Cmd decode(bool cke, bool ncs, bool nras, bool ncas, bool nwe) {
    if (!cke) return Cmd::CkeLow;
    if (ncs) return Cmd::Deselect;
    const unsigned bits = (static_cast<unsigned>(nras) << 2) |
                          (static_cast<unsigned>(ncas) << 1) |
                          static_cast<unsigned>(nwe);
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

bool is_data_command(Cmd cmd) {
    return cmd == Cmd::Active || cmd == Cmd::Read || cmd == Cmd::Write;
}

bool startup_ok(uint64_t cycles, uint64_t hz) {
    return cycles * 1'000'000ULL >= hz * 100ULL;
}

bool refresh_ok(uint64_t refresh_cycles, uint64_t hz) {
    const uint64_t period_cycles = refresh_cycles + 1;
    return period_cycles * 8192ULL * 1'000'000ULL <= hz * 64'000ULL;
}

bool cas_ok(uint64_t cas_latency, uint64_t hz) {
    return cas_latency >= 2 && (hz <= 100'000'000ULL || cas_latency >= 3);
}

void tick(Vsdram_startup_top& top, uint64_t& cycle) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
    ++cycle;
    top.clk = 0;
    top.eval();
}

}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);

    bool inject_early_read = false;
    bool force_ready_init_high = false;
    uint64_t freq_hz = 100'000'000ULL;
    bool legacy_constant_check = false;
    bool constants_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--inject-early-read") {
            inject_early_read = true;
        } else if (arg == "--force-ready-init-high") {
            force_ready_init_high = true;
        } else if (arg.rfind("--freq-hz=", 0) == 0) {
            freq_hz = std::stoull(arg.substr(std::string("--freq-hz=").size()));
        } else if (arg == "--check-legacy-100mhz-constants") {
            legacy_constant_check = true;
        } else if (arg == "--constants-only") {
            constants_only = true;
        }
    }

    if (legacy_constant_check) {
        constexpr uint64_t legacy_startup = 12'100;
        constexpr uint64_t legacy_refresh = 780;
        bool ok = true;
        if (!startup_ok(legacy_startup, freq_hz)) {
            std::cerr << "FAIL: legacy startup cycles " << legacy_startup
                      << " are below 100us at " << freq_hz << " Hz\n";
            ok = false;
        }
        if (!refresh_ok(legacy_refresh, freq_hz)) {
            std::cerr << "FAIL: legacy refresh cycles " << legacy_refresh
                      << " exceed the 64ms/8192-row interval at "
                      << freq_hz << " Hz\n";
            ok = false;
        }
        if (ok) {
            std::cout << "PASS: legacy constants satisfy timing at "
                      << freq_hz << " Hz\n";
        }
        return ok ? 0 : 1;
    }

    Vsdram_startup_top top;
    uint64_t cycle = 0;
    int64_t reset_release_cycle = -1;
    bool saw_mrs = false;
    bool violation = false;
    uint64_t violation_cycle = 0;
    Cmd violation_cmd = Cmd::Other;
    int64_t first_ready_cycle = -1;
    int64_t first_request_cycle = -1;
    std::vector<Event> events;

    top.force_ready_init_high = force_ready_init_high ? 1 : 0;
    top.reset = 1;
    top.pll_locked = 0;
    top.clk = 0;
    top.eval();
    const uint64_t startup_cycles = top.startup_cycles;
    const uint64_t refresh_cycles = top.refresh_cycles;
    const uint64_t cas_latency = top.configured_cas_latency;
    if (!startup_ok(startup_cycles, freq_hz)) {
        std::cerr << "FAIL: computed startup cycles " << startup_cycles
                  << " are below 100us at " << freq_hz << " Hz\n";
        return 3;
    }
    if (!refresh_ok(refresh_cycles, freq_hz)) {
        std::cerr << "FAIL: computed refresh cycles " << refresh_cycles
                  << " exceed the 64ms/8192-row interval at "
                  << freq_hz << " Hz\n";
        return 4;
    }
    if (!cas_ok(cas_latency, freq_hz)) {
        std::cerr << "FAIL: configured CAS latency " << cas_latency
                  << " is not valid for " << freq_hz << " Hz\n";
        return 5;
    }
    std::cout << "Computed timing constants: SDRAM_CLK_HZ=" << freq_hz
              << " startup_cycles=" << startup_cycles
              << " refresh_cycles=" << refresh_cycles
              << " cas_latency=" << cas_latency << "\n";
    if (constants_only) {
        std::cout << "PASS: computed startup/refresh constants satisfy SDRAM timing\n";
        return 0;
    }
    for (int i = 0; i < 8; ++i) tick(top, cycle);

    top.reset = 0;
    top.pll_locked = 1;
    reset_release_cycle = static_cast<int64_t>(cycle);

    constexpr uint64_t max_cycles = 13050;
    for (; cycle < max_cycles; ) {
        tick(top, cycle);

        bool cke = top.SDRAM_CKE;
        bool ncs = top.SDRAM_nCS;
        bool nras = top.SDRAM_nRAS;
        bool ncas = top.SDRAM_nCAS;
        bool nwe = top.SDRAM_nWE;
        bool injected = false;

        if (inject_early_read &&
            reset_release_cycle >= 0 &&
            cycle == static_cast<uint64_t>(reset_release_cycle + 10)) {
            ncs = false;
            nras = true;
            ncas = false;
            nwe = true;
            injected = true;
        }

        const Cmd cmd = decode(cke, ncs, nras, ncas, nwe);
        if (first_ready_cycle < 0 && top.sdram_ready) {
            first_ready_cycle = static_cast<int64_t>(cycle);
        }
        if (first_request_cycle < 0 && top.sdram_sel && (top.sdram_rd || top.sdram_wr)) {
            first_request_cycle = static_cast<int64_t>(cycle);
        }
        if (cmd != Cmd::Nop && cmd != Cmd::Deselect) {
            if (events.size() < 40) {
                events.push_back(Event{cycle, cmd, injected});
            }
            if (cmd == Cmd::ModeRegisterSet) {
                saw_mrs = true;
            }
            if (!saw_mrs && is_data_command(cmd) && !violation) {
                violation = true;
                violation_cycle = cycle;
                violation_cmd = cmd;
            }
        }
    }

    std::cout << "SDRAM startup command trace (first " << events.size()
              << " non-NOP/non-DESELECT commands after reset release at cycle "
              << reset_release_cycle << ")\n";
    for (const auto& e : events) {
        std::cout << "  cycle " << e.cycle << ": " << name(e.cmd);
        if (e.injected) std::cout << " [INJECTED]";
        std::cout << "\n";
    }
    std::cout << "First sdram_ready observed at cycle " << first_ready_cycle << "\n";
    std::cout << "First memtest sel+(rd|wr) observed at cycle " << first_request_cycle << "\n";

    if (!saw_mrs) {
        std::cerr << "FAIL: no MODE_REGISTER_SET observed before timeout\n";
        return 2;
    }
    if (violation) {
        std::cerr << "FAIL: " << name(violation_cmd)
                  << " before first MODE_REGISTER_SET at cycle "
                  << violation_cycle << "\n";
        return 1;
    }

    std::cout << "PASS: no ACTIVE/READ/WRITE before first MODE_REGISTER_SET\n";
    return 0;
}
