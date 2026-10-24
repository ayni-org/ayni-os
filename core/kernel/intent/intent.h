// Intents in the kernel's own form (C-WIRE-014). The C ABI decodes into it
// here, the network decoder (MA-038) will build the same values, and both
// pass check_form() and then the one semantic validation in apply_intent()
// (work_model.h), so the two paths cannot disagree. Views point into the
// caller's memory and are valid during the call only.

#ifndef AYNI_CORE_KERNEL_INTENT_INTENT_H
#define AYNI_CORE_KERNEL_INTENT_INTENT_H

#include "kernel/command/command_approval.h"
#include "kernel/command/command_reauthorization.h"
#include "kernel/command/command_task_limit.h"
#include "kernel/command/work_authority_result.h"

#include <ayni/authority.h>

#include <cstdint>
#include <optional>

namespace ayni::kernel {

struct Intent {
    ayni_intent_kind_t kind; // one of the kinds implemented
    uint64_t command_id;     // 0: not a command (command_table.h)
    uint64_t session_id;
    uint32_t expected_revision;
    ayni_grant_create_v1 grant_create;   // GRANT_CREATE, borrowed nested views
    GoalCreate create;                   // GOAL_CREATE
    GoalApproval approval;               // GOAL_APPROVE, including borrowed original views.
    GoalReauthorization reauthorization; // GOAL_REAUTHORIZE, including exact authority CAS.
    uint32_t model_call_limit;           // TASK_RAISE_MODEL_LIMIT: new positive total ceiling.
    uint64_t target_id; // target intents: the goal, task, instance or Grant they name
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
    ayni_intent_phase_t phase = AYNI_INTENT_APPLIED;
    ayni_intent_effect_t effect = AYNI_INTENT_EFFECT_CHANGED;
    uint64_t applied_boot_epoch = 0;
    // The goals' due work may have come or come sooner: ayni_submit_intent
    // wakes the owner (ADR-0018, ADR-0020).
    bool due_work;
    // A command accepted before: the result is its first, and nothing was
    // applied now.
    bool retried;
    // Present only for a newly accepted approval/reauthorization; copied into
    // the same command window before this transition is journaled.
    std::optional<WorkAuthorityResult> authorization;
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
