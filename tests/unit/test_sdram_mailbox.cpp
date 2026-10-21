#include "libmisterplex/sdram_mailbox.hpp"

#include <cstdio>
#include <cstdlib>

using namespace misterplex::sdram_mailbox;

namespace {
void require(bool ok, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "%s\n", msg);
        std::exit(1);
    }
}
} // namespace

int main() {
    const std::uint64_t summary_raw =
        (std::uint64_t{0xffff} << 48) |
        (std::uint64_t{5} << 44) |
        (std::uint64_t{7} << 40) |
        (std::uint64_t{0x48} << 32) |
        kSummaryMagic;

    Summary summary;
    require(decode_summary(summary_raw, summary), "failed to decode valid PLXM summary");
    require(summary.seq == 0x48, "decoded wrong PLXM seq");
    require(summary.state == 7, "decoded wrong PLXM state");
    require(summary.size_code == 5, "decoded wrong PLXM size");
    require(summary.error_count == 0xffff, "decoded wrong PLXM errors");

    Summary bad_summary;
    require(!decode_summary(summary_raw ^ 0x1, bad_summary), "accepted invalid PLXM magic");

    Diag diag_in;
    diag_in.read_sample = 0x0000;
    diag_in.first_fail_valid = true;
    diag_in.first_fail_addr = 0;
    diag_in.expected = 0x1357;

    Diag diag_out;
    const std::uint64_t diag_raw = encode_diag(diag_in);
    require(decode_diag(diag_raw, diag_out), "failed to decode valid PLXM diag");
    require(diag_out.version == kDiagVersion, "decoded wrong PLXM diag version");
    require(diag_out.read_sample == 0x0000, "decoded wrong PLXM read sample");
    require(diag_out.first_fail_valid, "decoded missing PLXM first-fail valid");
    require(diag_out.first_fail_addr == 0, "decoded wrong PLXM first-fail addr");
    require(diag_out.expected == 0x1357, "decoded wrong PLXM expected value");

    Diag bad_diag;
    require(!decode_diag((diag_raw & ~std::uint64_t{0x1f}) | 2, bad_diag),
            "accepted invalid PLXM diag version");

    std::printf("test_sdram_mailbox: OK (decoded PLXM summary and diag; invalid magic/version rejected)\n");
    return 0;
}
