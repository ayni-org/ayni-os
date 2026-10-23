// Shared Grant scalar rules (ADR-0030). Admission may recognize features that
// this version cannot execute; persisted facts contain only supported flags.
#ifndef AYNI_CORE_AUTHORITY_GRANT_GRANT_RULES_H
#define AYNI_CORE_AUTHORITY_GRANT_GRANT_RULES_H

#include <ayni/authority.h>

#include "foundation/kernel_id.h"

#include <cstdint>

namespace ayni::authority {

// ABI request flags; Scene/Quota are recognized but require later work packages.
constexpr uint32_t grant_deferred_flags = AYNI_GRANT_REQUIRES_SCENE | AYNI_GRANT_REQUIRES_QUOTA;
constexpr uint32_t grant_known_flags = AYNI_GRANT_ALLOW_DELEGATION | grant_deferred_flags;
// records.md Grant flags: only delegation can appear in an accepted fact.
constexpr uint32_t grant_persisted_flags = AYNI_GRANT_ALLOW_DELEGATION;
// No live deadline: permanent facts, or restored finite facts that cannot run.
constexpr uint64_t no_grant_deadline_us = 0;

// Unknown request bits are malformed, distinct from recognized deferred bits.
inline bool valid_grant_flags(uint32_t flags) {
    return (flags & ~grant_known_flags) == 0U;
}
// A recognized Scene/Quota requirement must fail explicitly as unsupported.
inline bool needs_deferred_grant_feature(uint32_t flags) {
    return (flags & grant_deferred_flags) != 0U;
}
// A durable accepted Grant/mapping cannot contain deferred or unknown bits.
inline bool valid_persisted_grant_flags(uint32_t flags) {
    return (flags & ~grant_persisted_flags) == 0U;
}
// Permanent uses zero duration; CURRENT_BOOT uses a positive duration in us.
inline bool valid_grant_lifetime(ayni_grant_lifetime_t lifetime, uint64_t duration_us) {
    return (lifetime == AYNI_GRANT_PERMANENT && duration_us == 0U) ||
           (lifetime == AYNI_GRANT_CURRENT_BOOT && duration_us != 0U);
}

// ADR-0037: Goal-specific authority is a nondelegable root; 0 is generic.
inline bool valid_goal_binding(uint64_t goal_id, uint64_t parent_id, uint32_t flags) {
    return goal_id == 0U || (foundation::is_kernel_id(goal_id) && parent_id == 0U && flags == 0U);
}

} // namespace ayni::authority
#endif // AYNI_CORE_AUTHORITY_GRANT_GRANT_RULES_H
