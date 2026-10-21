#pragma once

#include <cstdint>
#include "mailbox_abi_spec.hpp"

namespace misterplex::sdram_mailbox {

constexpr std::uint32_t kSummaryMagic = mailbox_abi::kPlxmMagic;
constexpr std::uintptr_t kSummaryOff = mailbox_abi::kPlxmOff; // on the build's PLX page
constexpr std::uintptr_t kDiagPhys = mailbox_abi::kSdramDiagAddr;
constexpr std::uint8_t kDiagVersion = 1;

struct Summary {
    std::uint8_t seq = 0;
    std::uint8_t state = 0;
    std::uint8_t size_code = 0;
    std::uint16_t error_count = 0;
};

struct Diag {
    std::uint8_t version = 0;
    std::uint16_t expected = 0;
    std::uint32_t first_fail_addr = 0;
    bool first_fail_valid = false;
    std::uint16_t read_sample = 0;
};

inline bool decode_summary(std::uint64_t raw, Summary& out) {
    if (static_cast<std::uint32_t>(raw) != kSummaryMagic)
        return false;
    out.seq = static_cast<std::uint8_t>((raw >> 32) & 0xff);
    out.state = static_cast<std::uint8_t>((raw >> 40) & 0x0f);
    out.size_code = static_cast<std::uint8_t>((raw >> 44) & 0x0f);
    out.error_count = static_cast<std::uint16_t>((raw >> 48) & 0xffff);
    return true;
}

inline bool decode_diag(std::uint64_t raw, Diag& out) {
    out.version = static_cast<std::uint8_t>(raw & 0x1f);
    if (out.version != kDiagVersion)
        return false;
    out.expected = static_cast<std::uint16_t>((raw >> 5) & 0xffff);
    out.first_fail_addr = static_cast<std::uint32_t>((raw >> 21) & 0x03ffffff);
    out.first_fail_valid = ((raw >> 47) & 1) != 0;
    out.read_sample = static_cast<std::uint16_t>((raw >> 48) & 0xffff);
    return true;
}

inline std::uint64_t encode_diag(const Diag& diag) {
    return (static_cast<std::uint64_t>(diag.read_sample) << 48) |
           (static_cast<std::uint64_t>(diag.first_fail_valid ? 1 : 0) << 47) |
           ((static_cast<std::uint64_t>(diag.first_fail_addr) & 0x03ffffff) << 21) |
           (static_cast<std::uint64_t>(diag.expected) << 5) |
           kDiagVersion;
}

} // namespace misterplex::sdram_mailbox
