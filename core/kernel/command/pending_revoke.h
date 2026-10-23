// Volatile accepted revoke controls. These outlive a restored command window
// during this Kernel's automatic reload, but are never journal/snapshot data.
#ifndef AYNI_CORE_KERNEL_COMMAND_PENDING_REVOKE_H
#define AYNI_CORE_KERNEL_COMMAND_PENDING_REVOKE_H

#include "kernel/command/command_canonical.h"

#include <array>
#include <cstdint>

namespace ayni::kernel {

// First-accept identity. Session/CAS are not consulted again after this copy.
struct AcceptedRevoke {
    uint64_t principal_id;      // Nonzero owner; 0 only in an unused PendingRevoke slot.
    uint64_t command_id;        // Nonzero identity in this principal's command space.
    uint32_t binding_revision;  // Nonzero owner revision captured when accepting.
    uint32_t expected_revision; // Original nonzero CAS, retained in canonical bytes.
    uint64_t target_grant_id;   // Nonzero original target, even if it disappears on reload.
};

// Owns an accepted identity and exact bytes until durable authority takes over.
struct PendingRevoke {
    AcceptedRevoke accepted{};                            // principal_id == 0 means a free slot
    std::array<uint8_t, revoke_command_size> canonical{}; // Fixed byte identity of accepted.
    // Both are zero while PENDING; both nonzero while APPLIED. No old
    // queued sequence can survive clear_restored() into another boot.
    uint64_t queued_boot_epoch = 0; // Writer boot, paired with queued_commit_seq.
    uint64_t queued_commit_seq = 0; // Applied transition sequence within that boot.
};

} // namespace ayni::kernel
#endif // AYNI_CORE_KERNEL_COMMAND_PENDING_REVOKE_H
