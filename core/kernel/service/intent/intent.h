// Intents in the kernel's own form (C-WIRE-014). The C ABI decodes into it
// here, the network decoder (MA-038) will build the same values, and both
// pass check_form() and then the one semantic validation in apply_intent()
// (work_model.h), so the two paths cannot disagree. Views point into the
// caller's memory and are valid during the call only.

#ifndef AYNI_CORE_KERNEL_INTENT_INTENT_H
#define AYNI_CORE_KERNEL_INTENT_INTENT_H

#include "kernel/service/command/command_approval.h"
#include "kernel/service/command/command_memory.h"
#include "kernel/service/command/command_reauthorization.h"
#include "kernel/service/command/command_scene.h"
#include "kernel/service/command/command_target.h"
#include "kernel/service/command/command_task_limit.h"
#include "kernel/service/command/work_authority_result.h"
#include "kernel/service/model/context.h"

#include <ayni/authority.h>

#include <cstdint>
#include <optional>

namespace ayni::kernel {

struct WorkModel;

// What ayni_submit_intent samples once for an intent; every rule reads
// these. Nothing has a default: a durable_seq left out would read as "all
// durable" and weaken the binding and Grant checks without a compile error.
struct IntentContext {
    uint64_t now_us;              // Host monotonic clock at entry; a new goal's schedule starts.
    uint64_t durable_seq;         // The current writer's acknowledged prefix at entry.
    bool has_room;                // ayni_kernel::storage_has_room(): a transition may start.
    ayni_status_t storage_status; // StorageBudget::admission(): AYNI_OK or AYNI_STORAGE_FAILED.
    OwnerClock clock;
    InputResumeSampler input;
};

struct Intent {
    ayni_intent_kind_t kind; // one of the kinds implemented
    uint64_t command_id;     // 0: not a command (command_table.h)
    uint64_t session_id;
    uint32_t expected_revision;
    ayni_grant_create_v1 grant_create;   // GRANT_CREATE, borrowed nested views
    GoalCreate create;                   // GOAL_CREATE
    GoalApproval approval;               // GOAL_APPROVE, including borrowed original views.
    GoalReauthorization reauthorization; // GOAL_REAUTHORIZE, including exact authority CAS.
    MemoryCommand memory;         // Owner memory save/confirm/delete, exact canonical semantics.
    authority::SceneChange scene; // SCENE_SET_ENABLED, copied complete command semantics.
    uint32_t model_call_limit;    // TASK_RAISE_MODEL_LIMIT: new positive total ceiling.
    uint64_t target_id;           // target intents: the goal, task, instance or Grant they name
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

// The argument rules that do not depend on kernel state: GOAL_CREATE has
// expected_revision 0, a goal type id that is a catalog id, parameter data
// unless their size is 0, a trigger kind this library defines with times
// that fit it, and a trigger source that is a catalog id for LOCAL_INPUT and
// empty otherwise. Every exact command kind (intent_traits.h) requires a
// nonzero command ID and its raw canonical request shape (independent of
// catalog and subjects). Returns INVALID_ARGUMENT for malformed input or
// UNSUPPORTED_VERSION for newer nested structs; changes no state.
ayni_status_t check_form(const Intent &intent);

// Decodes and checks an intent from the C ABI in the order of ayni.h,
// steps 1 and 2: struct sizes, reserved fields and the body, then
// unsupported kinds before body interpretation, then check_form(). On a
// failure, failure says why;
// out_intent is complete only on AYNI_OK.
ayni_status_t decode_intent(const ayni_intent_v1 &raw, Intent &out_intent, IntentFailure &failure);

// The digest of an ordinary command (intent_traits.h, C-WIRE-004): kind,
// expected revision and the body, not the session, so that a retry over
// another session, also after a restart, is the same command.
uint64_t ordinary_command_digest(const Intent &intent);

// Steps 4 and 5 of ayni_submit_intent: the session, the command id, then
// room for a transition and the rules of the intent's kind; applies the
// intent as one transition if they pass, and maps its command. The kernel
// is RUNNING. failure is reset, then says why on a refusal.
ayni_status_t apply_intent(WorkModel &model, const Intent &intent, const IntentContext &context,
                           IntentOutcome &outcome, IntentFailure &failure);

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_INTENT_INTENT_H
