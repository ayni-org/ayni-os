// Exact Goal approval identity, independent of live tables (ADR-0037).
// One codec owns the request's complete original creation and missing-item set.
#ifndef AYNI_CORE_KERNEL_COMMAND_COMMAND_APPROVAL_H
#define AYNI_CORE_KERNEL_COMMAND_COMMAND_APPROVAL_H

#include "kernel/service/command/command_goal_identity.h"
#include "kernel/service/command/exact_command.h"

#include <ayni/authority.h>

namespace ayni::kernel {

using work::GoalCreate;

// Borrowed exact approval identity. All views remain caller-owned through the
// call; lookup is independent of the Goal's current retention or catalog.
struct GoalApproval {
    uint64_t goal_id;               // Exact nonzero Kernel identity, not a reusable slot.
    uint32_t missing_mask;          // Original missing requirements by declared index.
    ayni_grant_lifetime_t lifetime; // Shared lifetime kind for this missing-item batch.
    uint64_t duration_us;           // Microseconds; 0 iff permanent.
    GoalCreate original;            // Immutable original creation inputs, never re-exported.
};

// records.md: scalars plus three u32 byte lengths, excluding their payloads.
constexpr uint32_t approval_command_prefix_size = 32;
constexpr uint32_t approval_command_fixed_size =
    approval_command_prefix_size + goal_identity_fixed_size;
constexpr uint32_t approval_command_max_bytes(uint32_t parameter_bytes) {
    return approval_command_prefix_size + goal_identity_max_bytes(parameter_bytes);
}
// Borrowed input/output view; reading borrows the canonical body's byte strings.
struct ApprovalCommand {
    uint32_t expected_revision; // Nonzero original CAS, independent of current revision.
    GoalApproval approval;      // Complete request; no live Goal/catalog lookup on retry.
};
// Shape and bounded lengths only; failure preserves out_size.
ayni_status_t measure_approval_command(const ApprovalCommand &input, uint32_t &out_size);
// Measures before writing; BUFFER_TOO_SMALL publishes needed size only.
ayni_status_t encode_approval_command(const ApprovalCommand &input, uint8_t *buffer,
                                      uint32_t capacity, uint32_t &out_size);
// Strict kind/layout/shape with no trailing bytes; atomic borrowed output.
bool read_approval_command(ayni_bytes_v1 bytes, ApprovalCommand &out);
template <> struct ExactCommand<ApprovalCommand> {
    static constexpr ayni_intent_kind_t kind(const ApprovalCommand &) {
        return AYNI_INTENT_GOAL_APPROVE;
    }
    static constexpr auto measure = measure_approval_command;
    static constexpr auto encode = encode_approval_command;
};

} // namespace ayni::kernel
#endif // AYNI_CORE_KERNEL_COMMAND_COMMAND_APPROVAL_H
