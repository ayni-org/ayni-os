// The state that intents read and change (ADR-0017): the stores of the work
// model, the id allocator and the transition clock. ayni_kernel holds one;
// the rules here take it instead of the whole kernel, so they cannot reach
// the Host, recovery or the event queue. Every function runs on the kernel
// owner thread, and every check runs before anything changes, so a refusal
// allocates no id and takes no transition; only AYNI_INTERNAL, a defect, may
// leave both used up.

#ifndef AYNI_CORE_KERNEL_INTENT_WORK_MODEL_H
#define AYNI_CORE_KERNEL_INTENT_WORK_MODEL_H

#include "authority/principal/principal_registry.h"
#include "catalog/goal_type/goal_type_registry.h"
#include "foundation/kernel_id.h"
#include "foundation/transition_stamp.h"
#include "kernel/intent/intent.h"
#include "kernel/kernel_config.h"
#include "kernel/transition_clock.h"
#include "work/agent/agent_table.h"
#include "work/goal/goal_table.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::kernel {

struct WorkModel {
    WorkModel(const KernelCapacities &capacities, const KernelLimits &limits,
              const KernelStorage &storage)
        : sessions(storage.sessions, capacities.session_capacity),
          goal_types(storage.goal_types, capacities.goal_type_capacity),
          agents(storage.agents, capacities.agent_capacity),
          goals(storage.goals, {capacities.goal_capacity, capacities.goal_parameter_max_bytes,
                                limits.goal_depth_max, limits.goal_children_max}) {}

    WorkModel(const WorkModel &) = delete;
    WorkModel &operator=(const WorkModel &) = delete;

    // Entering RUNNING: ids embed the durable boot epoch (C-ID-010), the
    // first transition takes first_stamp, and the instances of all agents
    // start in that one transition when there are any (C-LIFE-025).
    void start(uint64_t boot_epoch, foundation::TransitionStamp first_stamp);

    // Registration fills principals, goal types and agents in CONFIGURING;
    // the rest changes only in RUNNING.
    authority::PrincipalRegistry principals;
    authority::SessionTable sessions;
    catalog::GoalTypeRegistry goal_types;
    work::AgentTable agents;
    work::GoalTable goals;
    foundation::IdAllocator ids;
    TransitionClock transitions;
    // True while caller code runs inside one of these functions (a goal
    // type's validator). Caller code must not call the kernel; as a
    // fail-safe, owner-thread functions that change state then refuse to run
    // (ayni.h, C-POLICY-001), so nothing changes between a check and the
    // change it allows.
    bool calling_out = false;
};

// Opens a session for a registered principal: AYNI_OK with a new id,
// AYNI_NOT_FOUND for an unknown principal, AYNI_CAPACITY_EXCEEDED when the
// session table is full or this boot has no id left.
ayni_status_t open_session(WorkModel &model, uint64_t principal_id, ayni_channel_t channel,
                           uint64_t &out_session_id);

// Steps 4 and 5 of ayni_submit_intent: the session, then the rules of the
// intent's kind; applies the intent as one transition if they pass. The
// kernel is RUNNING. failure is reset, then says why on a refusal.
ayni_status_t apply_intent(WorkModel &model, const Intent &intent, IntentOutcome &outcome,
                           IntentFailure &failure);

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_INTENT_WORK_MODEL_H
