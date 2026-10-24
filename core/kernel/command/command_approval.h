// Exact Goal approval identity, independent of live tables (ADR-0037).
// One codec owns the request's complete original creation and missing-item set.
#ifndef AYNI_CORE_KERNEL_COMMAND_COMMAND_APPROVAL_H
#define AYNI_CORE_KERNEL_COMMAND_COMMAND_APPROVAL_H

#include "work/goal/goal_request.h"
#include "work/goal/goal_table.h"

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
constexpr uint32_t approval_command_fixed_size = 92;
// records.md: goal_type_id and trigger_source are the two bounded IDs.
constexpr uint32_t approval_catalog_id_count = 2;
constexpr uint32_t approval_command_max_bytes(uint32_t parameter_bytes) {
    return approval_command_fixed_size + approval_catalog_id_count * AYNI_CATALOG_ID_MAX_BYTES +
           parameter_bytes;
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
// Compare the immutable creation identity once for both live and restored proof.
bool matches_goal_creation(const GoalCreate &create, const work::Goal &goal,
                           ayni_bytes_v1 parameters);

} // namespace ayni::kernel
#endif // AYNI_CORE_KERNEL_COMMAND_COMMAND_APPROVAL_H
