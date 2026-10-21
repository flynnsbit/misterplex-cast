// Names the base daemon of test_core_gate_base_diff.sh (CORE_GATE_BASE_REF, default
// dbcc42fa, the prod daemon) still uses. This tree's mailbox_abi_spec.hpp keeps the
// frame-store mailboxes as page offsets only; the base computed doorbell_phys +
// (absolute - absolute), the same offsets. Force-included into the base build only.
#pragma once
#include "libmisterplex/input_mailbox.hpp"
#include "libmisterplex/mailbox_abi_spec.hpp"

namespace mailbox_abi {
inline constexpr uint32_t kPlxkAddr = 0x3007F000u; // the base tree's value
}

namespace misterplex {
inline constexpr uint32_t kDdrStatusMailboxPhys = mailbox_abi::kPlxkAddr + kDdrStatusMailboxOff;
inline constexpr uint32_t kInputMailboxPhys = mailbox_abi::kPlxkAddr + kInputMailboxOff;
} // namespace misterplex
