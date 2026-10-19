// Intents in the kernel's own form (C-WIRE-014). The C ABI decodes into it
// here, the network decoder (MA-038) will build the same values, and both
// pass check_form() and then the one semantic validation in apply_intent()
// (work_model.h), so the two paths cannot disagree. Views point into the
// caller's memory and are valid during the call only.

#ifndef AYNI_CORE_KERNEL_INTENT_INTENT_H
#define AYNI_CORE_KERNEL_INTENT_INTENT_H

#include "work/goal/trigger_schedule.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::kernel {

struct GoalCreate {
    ayni_string_v1 goal_type_id;
    uint32_t goal_type_version;
    uint64_t owner_instance_id;
    uint64_t parent_goal_id; // 0: a top-level goal
    ayni_bytes_v1 parameters;
    work::Trigger trigger;
};

struct Intent {
    ayni_intent_kind_t kind; // one of the kinds implemented
    uint64_t session_id;
    uint32_t expected_revision;
    GoalCreate create;  // GOAL_CREATE
    uint64_t target_id; // every other kind: the goal, task or instance it names
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
    bool derivation_due; // ayni_step may now derive a task (ADR-0018)
};

// Whether the kind is one this library implements.
bool is_implemented(ayni_intent_kind_t kind);

// The argument rules that do not depend on kernel state: GOAL_CREATE has
// expected_revision 0, a goal type id that is a catalog id, parameter data
// unless their size is 0, and a trigger kind this library defines. AYNI_OK
// or AYNI_INVALID_ARGUMENT.
ayni_status_t check_form(const Intent &intent);

// Decodes and checks an intent from the C ABI in the order of ayni.h,
// steps 1 and 2: struct sizes, reserved fields and the body, then
// check_form(), then unsupported kinds and command ids. On a failure, failure
// says why; out_intent is complete only on AYNI_OK.
ayni_status_t decode_intent(const ayni_intent_v1 &raw, Intent &out_intent, IntentFailure &failure);

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_INTENT_INTENT_H
