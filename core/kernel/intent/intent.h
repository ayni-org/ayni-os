// Intents in the kernel's own form (C-WIRE-014). The C ABI decodes into it
// here, the network decoder (MA-038) will build the same values, and both
// pass check_form() and then the one semantic validation in apply_intent()
// (work_model.h), so the two paths cannot disagree. Views point into the
// caller's memory and are valid during the call only.

#ifndef AYNI_CORE_KERNEL_INTENT_INTENT_H
#define AYNI_CORE_KERNEL_INTENT_INTENT_H

#include "work/goal/trigger_schedule.h"

#include <ayni/authority.h>

#include <cstdint>

namespace ayni::kernel {

struct GoalCreate {
    ayni_string_v1 goal_type_id;
    uint32_t goal_type_version;
    uint64_t owner_instance_id;
    uint64_t parent_goal_id; // 0: a top-level goal
    ayni_bytes_v1 parameters;
    work::Trigger trigger;
    uint64_t expiry_us;            // from creation; 0: never
    ayni_string_v1 trigger_source; // LOCAL_INPUT: the input it takes; empty otherwise
};

struct Intent {
    ayni_intent_kind_t kind; // one of the kinds implemented
    uint64_t command_id;     // 0: not a command (command_table.h)
    uint64_t session_id;
    uint32_t expected_revision;
    ayni_grant_create_v1 grant_create; // GRANT_CREATE, borrowed nested views
    GoalCreate create;                 // GOAL_CREATE
    uint64_t target_id;                // target intents: the goal, task or instance they name
};

// The detail of a refused intent, beside its status.
struct IntentFailure {
    ayni_reason_t reason;
    ayni_object_ref_v1 object;
};

// What an accepted intent changed.
struct IntentOutcome {
    ayni_object_ref_v1 object;
    uint64_t commit_seq;
    // The goals' due work may have come or come sooner: ayni_submit_intent
    // wakes the owner (ADR-0018, ADR-0020).
    bool due_work;
    // A command accepted before: the result is its first, and nothing was
    // applied now.
    bool retried;
};

// Whether the kind is one this library implements.
bool is_implemented(ayni_intent_kind_t kind);

// The argument rules that do not depend on kernel state: GOAL_CREATE has
// expected_revision 0, a goal type id that is a catalog id, parameter data
// unless their size is 0, a trigger kind this library defines with times
// that fit it, and a trigger source that is a catalog id for LOCAL_INPUT and
// empty otherwise. GRANT_CREATE requires a nonzero command ID and the raw
// canonical request shape (independent of catalog and subjects). Returns
// INVALID_ARGUMENT for malformed input or UNSUPPORTED_VERSION for newer
// nested structs; changes no state.
ayni_status_t check_form(const Intent &intent);

// Decodes and checks an intent from the C ABI in the order of ayni.h,
// steps 1 and 2: struct sizes, reserved fields and the body, then
// unsupported kinds before body interpretation, then check_form(). On a
// failure, failure says why;
// out_intent is complete only on AYNI_OK.
ayni_status_t decode_intent(const ayni_intent_v1 &raw, Intent &out_intent, IntentFailure &failure);

// The digest of an ordinary work command (kinds 1..8, C-WIRE-004): kind, expected
// revision and the body, not the session, so that a retry over another
// session, also after a restart, is the same command.
uint64_t ordinary_command_digest(const Intent &intent);

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_INTENT_INTENT_H
