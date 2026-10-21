#include "fake_plex_core.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

bool check(bool ok, const char* msg) {
    if (!ok)
        std::fprintf(stderr, "FAIL: %s\n", msg);
    return ok;
}

volatile uint64_t* capsWords(std::vector<uint8_t>& win) {
    return reinterpret_cast<volatile uint64_t*>(
        win.data() + (mailbox_abi::kVideoCapsAddr - fake_core::kRingWindowBase));
}

bool expectCaps(const fake_core::FakePlexCore::CapsConfig& cfg, volatile uint64_t* caps) {
    const uint64_t mpc1 = (static_cast<uint64_t>(cfg.features) << 32) |
                          mailbox_abi::kVideoFeaturesMagic;
    const uint64_t mpc2 = (static_cast<uint64_t>(cfg.max_height) << 48) |
                          (static_cast<uint64_t>(cfg.max_width) << 32) |
                          mailbox_abi::kVideoDimensionsMagic;
    const uint64_t mpc3 = (static_cast<uint64_t>(cfg.au_limit) << 32) |
                          mailbox_abi::kVideoAuLimitMagic;
    return check(caps[1] == mpc1, "MPC1 features word") &&
           check(caps[2] == mpc2, "MPC2 dimensions word") &&
           check(caps[3] == mpc3, "MPC3 AU limit word");
}

} // namespace

int main() {
    std::vector<uint8_t> regs(16);
    std::vector<uint8_t> win(fake_core::kRingWindowLen);
    fake_core::FakePlexCore core(regs.data(), win.data());
    volatile uint64_t* caps = capsWords(win);

    fake_core::FakePlexCore::CapsConfig defaults;
    core.reprogram(0x48005D45u);
    core.answerProbeForTest(0x1122334455667788ull);
    if (!expectCaps(defaults, caps))
        return 1;
    if (!check(caps[4] == ((0x48005D45ull << 32) | mailbox_abi::kVideoBuildMagic),
               "MPC4 build word"))
        return 1;

    fake_core::FakePlexCore::CapsConfig override;
    override.features = 0x0000A19Eu; // deliberately clears a required bit.
    override.max_width = 720;
    override.max_height = 480;
    override.au_limit = 65536;
    core.setCapsConfig(override);
    core.reprogram(0x4801F40Au);
    core.answerProbeForTest(0x8877665544332211ull);
    if (!expectCaps(override, caps))
        return 1;
    if (!check(caps[4] == ((0x4801F40Aull << 32) | mailbox_abi::kVideoBuildMagic),
               "override MPC4 build word"))
        return 1;
    if (!check((override.features & fake_core::ring::kRequiredVideoFeatures) !=
                   fake_core::ring::kRequiredVideoFeatures,
               "override may clear required features"))
        return 1;

    return 0;
}
