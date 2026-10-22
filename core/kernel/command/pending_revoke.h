// Volatile accepted revoke controls. These outlive a restored command window
// during this Kernel's automatic reload, but are never journal/snapshot data.
#ifndef AYNI_CORE_KERNEL_COMMAND_PENDING_REVOKE_H
#define AYNI_CORE_KERNEL_COMMAND_PENDING_REVOKE_H

#include "kernel/command/command_canonical.h"

#include <array>
#include <cstdint>

namespace ayni::kernel {

struct AcceptedRevoke {
    uint64_t principal_id;
    uint64_t command_id;
    uint32_t binding_revision;
    uint32_t expected_revision;
    uint64_t target_grant_id;
};

struct PendingRevoke {
    AcceptedRevoke accepted{}; // principal_id == 0 means a free slot
    std::array<uint8_t, revoke_command_size> canonical{};
    // Both are zero while PENDING; both nonzero while APPLIED. No old
    // queued sequence can survive clear_restored() into another boot.
    uint64_t queued_boot_epoch = 0;
    uint64_t queued_commit_seq = 0;
};

} // namespace ayni::kernel
#endif // AYNI_CORE_KERNEL_COMMAND_PENDING_REVOKE_H
