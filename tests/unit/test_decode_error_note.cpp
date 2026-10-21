// T-DAEMON-MVPS (review 18:34): on the AU push-reject session end the daemon names the core's
// decode_error (media_player.cpp, AU push reject -> failFpgaSession). Cases, each through the MVPS words the
// core publishes (decodeVideoPresentation) and then ddr_bitstream_ring::decodeErrorNote:
//   decode_error 22 -> " decode_error=22 (sub-8x8 unsupported)"; an unknown code (99) -> " decode_error=99";
//   no code (no error flag, error without a code, a stale code without the flag, an undecodable read) -> "".
#include "libmisterplex/ddr_bitstream_ring.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {
namespace ring = misterplex::ddr_bitstream_ring;
namespace abi = ::mailbox_abi;

int g_fails = 0;
int g_checks = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_fails;                                                         \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                   \
            std::printf(__VA_ARGS__);                                          \
            std::printf("\n");                                                 \
        }                                                                      \
    } while (0)

constexpr uint64_t kSession = 0x5ab8ull;
constexpr uint64_t kNonce = 0x22aa55ull;

std::array<uint64_t, 9> mvps(uint32_t flags, uint32_t code) {
    std::array<uint64_t, 9> w{};
    w[0] = (static_cast<uint64_t>(flags) << 32) | abi::kVideoPresentationMagic;
    w[1] = kSession;
    w[6] = kNonce;
    w[7] = static_cast<uint64_t>(code) << 32;   // fpga_video_publish.sv status word {24'd0, error_code, 32'd0}
    w[8] = (uint64_t{1} << 32) | abi::kVideoPresentationCommitMagic;
    return w;
}

// The media_player reject path: the note is appended only when the MVPS read decodes.
std::string rejectLine(const std::array<uint64_t, 9>& words, uint64_t nonce = kNonce) {
    ring::VideoPresentation p{};
    const std::string note = ring::decodeVideoPresentation(words, kSession, nonce, p) ? ring::decodeErrorNote(p)
                                                                                     : std::string();
    return "AU transport rejected sequence 7" + note;
}
}  // namespace

int main() {
    const uint32_t err = ring::kVideoActive | ring::kVideoError;
    const std::string base = "AU transport rejected sequence 7";
    const std::string named = rejectLine(mvps(err, 22));
    CHECK(named == base + " decode_error=22 (sub-8x8 unsupported)", "code 22: '%s'", named.c_str());
    const std::string unknown = rejectLine(mvps(err, 99));
    CHECK(unknown == base + " decode_error=99", "unknown code 99: '%s'", unknown.c_str());
    const std::string known = rejectLine(mvps(err, 20));
    CHECK(known == base + " decode_error=20", "code 20 stays generic: '%s'", known.c_str());
    CHECK(rejectLine(mvps(ring::kVideoActive, 0)) == base, "no error, no code: changed");
    CHECK(rejectLine(mvps(err, 0)) == base, "error without a code: changed");
    CHECK(rejectLine(mvps(ring::kVideoActive, 22)) == base, "a stale code without the error flag: changed");
    CHECK(rejectLine(mvps(err, 22), kNonce + 1) == base, "an undecodable read (other nonce): changed");
    if (g_fails) {
        std::printf("test_decode_error_note: %d FAIL of %d checks\n", g_fails, g_checks);
        return 1;
    }
    std::printf("test_decode_error_note: OK (%d checks; 22 named, 99/20 generic, no code unchanged)\n", g_checks);
    std::printf("  %s\n  %s\n", named.c_str(), unknown.c_str());
    return 0;
}
