#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
constexpr uint8_t kSizeUnknown = 0;
constexpr uint8_t kSize16 = 2;
constexpr uint8_t kSize32 = 3;
constexpr uint8_t kSize64 = 4;
constexpr uint8_t kSize128 = 5;

constexpr uint32_t kWords16 = 16;
constexpr uint32_t kWords32 = 32;
constexpr uint32_t kWords64 = 64;
constexpr uint32_t kWords128 = 128;
constexpr uint16_t kPat0 = 0x1357;
constexpr uint16_t kPat16 = 0x5aa5;
constexpr uint16_t kPat32 = 0xc33c;
constexpr uint16_t kPat64 = 0x9e81;
constexpr uint32_t kNoFail = 0xffffffffu;

enum State : uint8_t {
    ST_RESET = 0,
    ST_DET_W16 = 2,
    ST_DET_W32 = 3,
    ST_DET_R0 = 4,
    ST_DET_R16 = 5,
    ST_DET_PICK = 6,
    ST_W1_WRITE = 7,
    ST_W1_READ = 8,
    ST_W0_WRITE = 9,
    ST_W0_READ = 10,
    ST_ADDR_WRITE = 11,
    ST_ADDR_READ = 12,
    ST_DONE = 13,
    ST_OP_ISSUE = 14,
    ST_OP_DROP = 15,
    ST_OP_WAIT = 16,
    ST_DET_W64 = 17,
    ST_DET_R32 = 18,
    ST_DET_R64 = 19,
};

uint16_t walk_one(uint32_t addr) {
    return static_cast<uint16_t>(1u << (addr & 0xf));
}

uint16_t addr_pattern(uint32_t addr) {
    return static_cast<uint16_t>((addr & 0xffffu) ^ ((addr >> 16) & 0x03ffu) ^ 0xa5a5u);
}

struct Memtest {
    bool reset = true;
    bool sel = false;
    uint32_t addr = 0;
    uint16_t din = 0;
    bool wr = false;
    bool rd = false;
    uint8_t bs = 0x3;
    bool refresh = false;
    uint8_t state_code = 1;
    uint8_t size_code = kSizeUnknown;
    uint16_t error_count = 0;
    bool done = false;
    bool pass = false;
    uint32_t first_fail_addr = kNoFail;
    uint16_t first_fail_got = 0;
    uint16_t first_fail_want = 0;

    State state = ST_RESET;
    State op_return = ST_RESET;
    uint32_t ptr = 0;
    uint32_t limit_words = kWords32;
    uint32_t op_addr = 0;
    uint16_t op_din = 0;
    uint16_t op_expect = 0;
    bool op_write = false;
    bool op_check = false;
    uint16_t det_r0 = 0;
    uint16_t det_r16 = 0;
    uint16_t det_r32 = 0;
    uint16_t last_read = 0;
    uint16_t refresh_ctr = 0;

    void start_write(uint32_t a, uint16_t d, State ret) {
        op_addr = a;
        op_din = d;
        op_expect = 0;
        op_write = true;
        op_check = false;
        op_return = ret;
        state = ST_OP_ISSUE;
    }

    void start_read(uint32_t a, uint16_t exp, bool check, State ret) {
        op_addr = a;
        op_din = 0;
        op_expect = exp;
        op_write = false;
        op_check = check;
        op_return = ret;
        state = ST_OP_ISSUE;
    }

    void tick(uint16_t sdram_dout, bool sdram_ready) {
        if (reset) {
            refresh_ctr = 0;
            refresh = false;
            state = ST_RESET;
            state_code = 1;
            size_code = kSizeUnknown;
            error_count = 0;
            done = false;
            pass = false;
            first_fail_addr = kNoFail;
            first_fail_got = 0;
            first_fail_want = 0;
            sel = false;
            wr = false;
            rd = false;
            addr = 0;
            din = 0;
            ptr = 0;
            limit_words = kWords32;
            det_r0 = 0;
            det_r16 = 0;
            det_r32 = 0;
            last_read = 0;
            op_addr = 0;
            op_din = 0;
            op_expect = 0;
            op_write = false;
            op_check = false;
            op_return = ST_RESET;
            return;
        }

        if (refresh_ctr == 7) {
            refresh_ctr = 0;
            refresh = !refresh;
        } else {
            ++refresh_ctr;
        }

        sel = false;
        wr = false;
        rd = false;

        switch (state) {
        case ST_RESET:
            state_code = 1;
            if (sdram_ready) {
                state_code = 2;
                start_write(0, kPat0, ST_DET_W16);
            }
            break;
        case ST_DET_W16:
            start_write(kWords16, kPat16, ST_DET_W32);
            break;
        case ST_DET_W32:
            start_write(kWords32, kPat32, ST_DET_W64);
            break;
        case ST_DET_W64:
            start_write(kWords64, kPat64, ST_DET_R0);
            break;
        case ST_DET_R0:
            start_read(0, 0, false, ST_DET_R16);
            break;
        case ST_DET_R16:
            det_r0 = last_read;
            start_read(kWords16, 0, false, ST_DET_R32);
            break;
        case ST_DET_R32:
            det_r16 = last_read;
            start_read(kWords32, 0, false, ST_DET_R64);
            break;
        case ST_DET_R64:
            det_r32 = last_read;
            start_read(kWords64, 0, false, ST_DET_PICK);
            break;
        case ST_DET_PICK:
            if (det_r0 == kPat0 && det_r16 == kPat16 && det_r32 == kPat32 && last_read == kPat64) {
                size_code = kSize128;
                limit_words = kWords128;
            } else if (det_r0 == kPat0 && det_r16 == kPat16 && det_r32 == kPat32) {
                size_code = kSize64;
                limit_words = kWords64;
            } else if (det_r16 == kPat16) {
                size_code = kSize32;
                limit_words = kWords32;
            } else if (det_r0 == kPat32 || det_r16 == kPat32 || det_r32 == kPat32) {
                size_code = kSize16;
                limit_words = kWords16;
            } else {
                size_code = kSizeUnknown;
                limit_words = kWords16;
                if (first_fail_addr == kNoFail) {
                    first_fail_addr = 0;
                    first_fail_got = det_r0;
                    first_fail_want = kPat0;
                }
                if (error_count != 0xffff)
                    ++error_count;
            }
            ptr = 0;
            state_code = 3;
            state = ST_W1_WRITE;
            break;
        case ST_W1_WRITE:
            if (ptr < limit_words) {
                start_write(ptr, walk_one(ptr), ST_W1_WRITE);
                ++ptr;
            } else {
                ptr = 0;
                state = ST_W1_READ;
            }
            break;
        case ST_W1_READ:
            if (ptr < limit_words) {
                start_read(ptr, walk_one(ptr), true, ST_W1_READ);
                ++ptr;
            } else {
                ptr = 0;
                state_code = 4;
                state = ST_W0_WRITE;
            }
            break;
        case ST_W0_WRITE:
            if (ptr < limit_words) {
                start_write(ptr, static_cast<uint16_t>(~walk_one(ptr)), ST_W0_WRITE);
                ++ptr;
            } else {
                ptr = 0;
                state = ST_W0_READ;
            }
            break;
        case ST_W0_READ:
            if (ptr < limit_words) {
                start_read(ptr, static_cast<uint16_t>(~walk_one(ptr)), true, ST_W0_READ);
                ++ptr;
            } else {
                ptr = 0;
                state_code = 5;
                state = ST_ADDR_WRITE;
            }
            break;
        case ST_ADDR_WRITE:
            if (ptr < limit_words) {
                start_write(ptr, addr_pattern(ptr), ST_ADDR_WRITE);
                ++ptr;
            } else {
                ptr = 0;
                state = ST_ADDR_READ;
            }
            break;
        case ST_ADDR_READ:
            if (ptr < limit_words) {
                start_read(ptr, addr_pattern(ptr), true, ST_ADDR_READ);
                ++ptr;
            } else {
                state = ST_DONE;
            }
            break;
        case ST_DONE:
            done = true;
            pass = (error_count == 0) && (size_code != kSizeUnknown);
            state_code = pass ? 6 : 7;
            break;
        case ST_OP_ISSUE:
            if (sdram_ready) {
                sel = true;
                addr = op_addr;
                din = op_din;
                wr = op_write;
                rd = !op_write;
                state = ST_OP_DROP;
            }
            break;
        case ST_OP_DROP:
            state = ST_OP_WAIT;
            break;
        case ST_OP_WAIT:
            if (sdram_ready) {
                last_read = sdram_dout;
                if (op_check && sdram_dout != op_expect) {
                    if (first_fail_addr == kNoFail) {
                        first_fail_addr = op_addr;
                        first_fail_got = sdram_dout;
                        first_fail_want = op_expect;
                    }
                    if (error_count != 0xffff)
                        ++error_count;
                }
                state = op_return;
            }
            break;
        default:
            state = ST_RESET;
            break;
        }
    }
};

struct Controller {
    enum class Fault {
        None,
        CorruptOneRead,
        ReadNextBurstCell,
        Chip1Unrefreshed,
        Addr26StuckLow,
    };

    explicit Controller(uint32_t words, Fault fault = Fault::None)
        : mem(words), fault_mode(fault) {}

    std::vector<uint16_t> mem;
    Fault fault_mode = Fault::None;
    bool ready = true;
    uint16_t dout = 0;
    int busy = 0;
    bool pending_read = false;
    uint32_t pending_addr = 0;
    bool saw_bad_byte_enable = false;
    bool touched_chip1 = false;
    uint32_t max_addr = 0;

    uint32_t phys(uint32_t addr) const {
        if (fault_mode == Fault::Addr26StuckLow)
            addr %= kWords64;
        return addr % static_cast<uint32_t>(mem.size());
    }

    bool upper_chip_absent(uint32_t addr) const {
        return mem.size() <= kWords64 && addr >= kWords64;
    }

    uint16_t read_word(uint32_t addr) const {
        if (fault_mode == Fault::ReadNextBurstCell)
            addr += 1;
        if (fault_mode == Fault::Chip1Unrefreshed && addr >= kWords64)
            return 0xffff;
        if (upper_chip_absent(addr))
            return 0xffff;
        uint16_t value = mem[phys(addr)];
        if (fault_mode == Fault::CorruptOneRead && phys(addr) == 5)
            value ^= 0x0001;
        return value;
    }

    void tick(const Memtest& t) {
        if (busy > 0) {
            --busy;
            if (busy == 0) {
                if (pending_read) {
                    dout = read_word(pending_addr);
                    pending_read = false;
                }
                ready = true;
            }
            return;
        }

        ready = true;
        if (!t.sel || (!t.wr && !t.rd))
            return;

        if (t.bs != 0x3)
            saw_bad_byte_enable = true;
        if (t.addr >= kWords64)
            touched_chip1 = true;
        if (t.addr > max_addr)
            max_addr = t.addr;

        ready = false;
        if (t.wr) {
            if (!upper_chip_absent(t.addr))
                mem[phys(t.addr)] = t.din;
            busy = 2;
        } else {
            pending_read = true;
            pending_addr = t.addr;
            busy = 3;
        }
    }
};

struct Result {
    uint8_t state_code = 0;
    uint8_t size_code = 0;
    uint16_t error_count = 0;
    bool pass = false;
    int cycles = 0;
    bool bad_byte_enable = false;
    bool touched_chip1 = false;
    uint32_t max_addr = 0;
    uint32_t first_fail_addr = kNoFail;
    uint16_t first_fail_got = 0;
    uint16_t first_fail_want = 0;
};

Result run(uint32_t memory_words, Controller::Fault fault = Controller::Fault::None) {
    Memtest t;
    Controller c(memory_words, fault);
    for (int i = 0; i < 2; ++i) {
        t.reset = true;
        t.tick(c.dout, c.ready);
        c.tick(t);
    }
    t.reset = false;

    for (int cycle = 0; cycle < 20000; ++cycle) {
        t.tick(c.dout, c.ready);
        c.tick(t);
        if (t.done)
            return {t.state_code, t.size_code, t.error_count, t.pass, cycle,
                    c.saw_bad_byte_enable, c.touched_chip1, c.max_addr,
                    t.first_fail_addr, t.first_fail_got, t.first_fail_want};
    }
    std::fprintf(stderr, "sdram memtest model timed out for %u words\n", memory_words);
    std::exit(1);
}

void require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
} // namespace

int main() {
    const Result good128 = run(kWords128);
    require(good128.pass, "known-good 128-word model did not PASS");
    require(good128.state_code == 6, "known-good model did not report state_code=6");
    require(good128.size_code == kSize128, "known-good model did not detect 128-word size");
    require(good128.error_count == 0, "known-good model reported errors");
    require(!good128.bad_byte_enable, "memtest drove byte enables other than 2'b11");
    require(good128.touched_chip1 && good128.max_addr >= (kWords128 - 1),
            "known-good 128-word model did not drive the full address range");

    const Result good64 = run(kWords64);
    require(good64.pass && good64.size_code == kSize64, "64-word alias probe failed");

    const Result good32 = run(kWords32);
    require(good32.pass && good32.size_code == kSize32, "32-word alias probe failed");

    const Result good16 = run(kWords16);
    require(good16.pass && good16.size_code == kSize16, "16-word alias probe failed");

    const Result corrupt128 = run(kWords128, Controller::Fault::CorruptOneRead);
    require(!corrupt128.pass, "corrupted model unexpectedly PASSed");
    require(corrupt128.state_code == 7, "corrupted model did not report state_code=7");
    require(corrupt128.size_code == kSize128, "corrupted model should still detect size");
    require(corrupt128.error_count != 0, "corrupted model did not count mismatches");
    require(!corrupt128.bad_byte_enable, "memtest drove byte enables other than 2'b11 on corrupt run");

    const Result burst_late = run(kWords128, Controller::Fault::ReadNextBurstCell);
    require(!burst_late.pass, "burst-offset model unexpectedly PASSed");
    require(burst_late.state_code == 7, "burst-offset model did not report state_code=7");
    require(burst_late.size_code == kSizeUnknown, "burst-offset model should make detection unknown");
    require(burst_late.first_fail_addr == 0, "burst-offset model should fail at the first low address");

    const Result chip1_refresh_gap = run(kWords128, Controller::Fault::Chip1Unrefreshed);
    require(chip1_refresh_gap.first_fail_addr >= kWords64 || chip1_refresh_gap.size_code == kSize64,
            "chip1 refresh gap should not look like an immediate low-address failure");

    const Result addr26_stuck = run(kWords128, Controller::Fault::Addr26StuckLow);
    require(addr26_stuck.size_code != kSize128, "addr26-stuck model should not detect 128MB");
    require(addr26_stuck.touched_chip1, "addr26-stuck model did not exercise the high chip address bit");

    std::printf("test_sdram_memtest_sim: OK (128MB model PASS size=%u max_addr=%u; corrupt 128MB FAIL errors=%u; burst-offset reproduces state=%u size=%u first_fail_addr=%u got=0x%04x want=0x%04x; chip1-refresh-gap size=%u first_fail=%u; addr26-stuck size=%u pass=%u; 64/32/16 alias probes OK)\n",
                good128.size_code, good128.max_addr, corrupt128.error_count,
                burst_late.state_code, burst_late.size_code, burst_late.first_fail_addr,
                burst_late.first_fail_got, burst_late.first_fail_want,
                chip1_refresh_gap.size_code, chip1_refresh_gap.first_fail_addr,
                addr26_stuck.size_code, addr26_stuck.pass ? 1 : 0);
    return 0;
}
