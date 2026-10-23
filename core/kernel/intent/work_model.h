// The state that intents read and change (ADR-0017, ADR-0018): the stores of
// the work model, the id allocator and the transition clock. ayni_kernel
// holds one; the rules here take it instead of the whole kernel, so they
// cannot reach the Host, recovery or the event queue. Every function runs on
// the kernel owner thread, and every check runs before anything changes, so
// a refusal allocates no id and takes no transition; only AYNI_INTERNAL, a
// defect, may leave both used up.

#ifndef AYNI_CORE_KERNEL_INTENT_WORK_MODEL_H
#define AYNI_CORE_KERNEL_INTENT_WORK_MODEL_H

#include "authority/grant/grant_table.h"
#include "authority/principal/principal_registry.h"
#include "catalog/capability/capability_registry.h"
#include "catalog/goal_type/goal_type_registry.h"
#include "catalog/strategy/strategy_registry.h"
#include "foundation/kernel_id.h"
#include "foundation/transition_stamp.h"
#include "kernel/command/command_canonical.h"
#include "kernel/command/command_table.h"
#include "kernel/intent/intent.h"
#include "kernel/kernel_config.h"
#include "kernel/transition_clock.h"
#include "work/agent/agent_table.h"
#include "work/goal/goal_authority.h"
#include "work/goal/goal_table.h"
#include "work/propagation.h"
#include "work/task/task_table.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::kernel {

// Narrow borrowed clock for admission and post-callback consumption. Production
// callers supply the Host clock; standalone model tests may use fixed now_us.
struct OwnerClock {
    void *ctx = nullptr;                     // Borrowed for this synchronous call only.
    uint64_t (*now_us)(void *ctx) = nullptr; // Monotonic microseconds; null uses call's sample.
};

struct WorkModel {
    WorkModel(const KernelCapacities &capacities, const KernelLimits &limits,
              const KernelStorage &storage)
        : sessions(storage.sessions, capacities.session_capacity),
          goal_types(storage.goal_types, capacities.goal_type_capacity),
          agents(storage.agents, capacities.agent_capacity),
          goals(storage.goals, {capacities.goal_capacity, capacities.goal_parameter_max_bytes,
                                limits.goal_depth_max, limits.goal_children_max}),
          tasks(storage.tasks, {capacities.task_capacity, capacities.task_step_capacity,
                                capacities.task_checkpoint_max_bytes}),
          strategies(storage.strategies, capacities.strategy_capacity),
          commands(storage.commands, authority::PrincipalRegistry::capacity,
                   capacities.command_capacity,
                   authority_command_max_bytes(capacities.authority_scope_max_bytes),
                   capacities.grant_capacity),
          capabilities(storage.capabilities, capacities.capability_capacity,
                       capacities.authority_scope_max_bytes),
          grants(storage.grants, {capacities.grant_capacity, capacities.authority_scope_max_bytes,
                                  capacities.grant_delegation_depth_max}),
          goal_authority(storage.goal_authority,
                         {capacities.goal_capacity, capacities.goal_requirement_capacity,
                          capacities.authority_scope_max_bytes}) {}

    WorkModel(const WorkModel &) = delete;
    WorkModel &operator=(const WorkModel &) = delete;

    // Entering RUNNING: ids embed the durable boot epoch (C-ID-010), the
    // first transition takes first_stamp, and the instances of all agents
    // still REGISTERED start in that one transition when there are any
    // (C-LIFE-025); the tasks recovery restored settle against their goals
    // and owners in it too (work::settle_restored). Returns whether there
    // was that transition.
    bool start(uint64_t boot_epoch, foundation::TransitionStamp first_stamp);

    work::WorkTables tables() { return {goals, tasks, agents, &goal_authority}; }
    // Export a staged immutable ceiling and select existing durable Grants.
    authority::GrantCheck prepare_goal_authority(const catalog::GoalType &type,
                                                 const GoalCreate &create, uint64_t now_us,
                                                 uint64_t durable_seq,
                                                 work::GoalAuthorityRecord &out, OwnerClock clock);
    // Check selected identities without selecting a replacement; caller owns time.
    authority::GrantCheck check_requirement(const work::GoalRequirement &requirement,
                                            uint64_t now_us, uint64_t durable_seq,
                                            uint64_t goal_id) const;
    // Includes the owning Goal and its bounded ancestor chain.
    authority::GrantCheck check_goal_authority(uint32_t index, uint64_t now_us,
                                               uint64_t durable_seq) const;
    // Persist authorization pause independently of user pause; settle reached Tasks.
    void pause_goal_authority(uint32_t index);
    // Validates authority once, then the work-owned subjects of that same path.
    authority::GrantCheck check_grant(uint32_t index, uint64_t now_us, uint64_t durable_seq,
                                      uint64_t goal_id = 0) const;
    // Consumes an unchanged successful GrantTable path; root admission's empty
    // parent path succeeds. No second parent lookup or authority revalidation.
    authority::GrantCheck
    check_grant_subjects(const authority::GrantTable::CheckedChain &chain) const;

    // Build a local mask from current work references. Terminal work stops
    // protecting its rows only after the current writer confirms its last stamp.
    authority::GrantReclaimExclusions grant_reclaim_exclusions(uint64_t durable_seq) const;
    // Hand off removed references after all fallible checks. Repeated releases
    // retain the latest fence; no Goal/Grant reference count is duplicated.
    void release_goal_references(const work::GoalAuthorityRecord &record, uint64_t commit_seq);
    // A replaced Goal slot discards exactly its former identity's metadata.
    void clear_goal_authority(uint32_t index, uint64_t previous_id, uint64_t commit_seq);
    // Own the cross-table replacement: work propagation, then reference handoff.
    bool insert_goal(const work::NewGoal &goal, uint64_t goal_id, foundation::TransitionStamp stamp,
                     uint32_t &out_index);

    // Registration fills principals, goal types, agents and strategies in
    // CONFIGURING; the rest changes only in RUNNING.
    authority::PrincipalRegistry principals;
    authority::SessionTable sessions;
    catalog::GoalTypeRegistry goal_types;
    work::AgentTable agents;
    work::GoalTable goals;
    work::TaskTable tasks;
    catalog::StrategyRegistry strategies;
    // The commands accepted, which the log keeps with their transitions.
    CommandTable commands;
    // Trusted, immutable authorization schemas; registration enables no I/O.
    catalog::CapabilityRegistry capabilities;
    authority::GrantTable grants;
    // Work-owned immutable ceilings and selected Grant identities; no live Grant cache.
    work::GoalAuthorityTable goal_authority;
    foundation::IdAllocator ids;
    TransitionClock transitions;
    // True while caller code runs inside one of these functions (a goal
    // type's validator or a strategy's decide function). Caller code must
    // not call the kernel; as a fail-safe, owner-thread functions that
    // change state then refuse to run (ayni.h, C-POLICY-001), so nothing
    // changes between a check and the change it allows.
    bool calling_out = false;
    // Set by a change to the goals' due work (which goals may advance or
    // expire, room in the task table, or a held task); cleared once a search
    // finds none due. That search also records when the next due work comes
    // and whether an id was left, so ayni_step searches again only after
    // such a change, once that time has come or once no id is left
    // (ADR-0019, ADR-0020, ADR-0022).
    bool due_work_changed = false;
    uint64_t next_due_work_us = work::no_time;
    bool searched_with_id = true;
};

// Opens for the unchanged registered Principal already checked by the ABI
// entry. AYNI_OK with a new id, or AYNI_CAPACITY_EXCEEDED when the session table
// is full or this boot has no id left; failure preserves out_session_id.
ayni_status_t open_session(WorkModel &model, const authority::Principal &principal,
                           ayni_channel_t channel, uint64_t &out_session_id);

// Steps 4 and 5 of ayni_submit_intent: the session, the command id, then
// room for a transition (has_room) and the rules of the intent's kind;
// applies the intent as one transition if they pass, and maps its command.
// The kernel is RUNNING; now_us is the host monotonic clock, from which a
// new goal's schedule starts. failure is reset, then says why on a refusal.
ayni_status_t apply_intent(WorkModel &model, const Intent &intent, uint64_t now_us, bool has_room,
                           IntentOutcome &outcome, IntentFailure &failure,
                           ayni_status_t storage_status = AYNI_OK,
                           uint64_t durable_seq = UINT64_MAX, OwnerClock clock = {});

// Records a local input from source as an occurrence, due at now_us, of
// every goal whose trigger takes it, all in one transition (ADR-0021).
// Returns false, changing nothing, when no goal takes it.
bool apply_local_input(WorkModel &model, const catalog::CatalogId &source, uint64_t now_us);

// Does the goals' next due work by now_us (work::find_due_work): expires a
// goal, derives a task or skips occurrences, each as one transition.
// Returns false, changing nothing, when none is due. An expiry that would
// exhaust a revision drops the goal's expiry instead (C-COMMIT-012).
bool do_due_work(WorkModel &model, uint64_t now_us, uint64_t authority_now_us,
                 uint64_t durable_seq);
// Whether do_due_work(now_us) would do something; out_next_us is then now_us.
// When it would not, clears due_work_changed and sets out_next_us to when the
// next due work comes (work::no_time: none will without a change). Scans the
// goal table, each goal along at most goal_depth_max ancestors, and the task
// table, only while due_work_changed is set, that time has come or the ids
// have just run out.
bool refresh_due_work(WorkModel &model, uint64_t now_us, uint64_t &out_next_us);

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_INTENT_WORK_MODEL_H
