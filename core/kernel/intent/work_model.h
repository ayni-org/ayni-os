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
#include "authority/scene/scene_table.h"
#include "catalog/capability/capability_registry.h"
#include "catalog/goal_type/goal_type_registry.h"
#include "catalog/strategy/strategy_registry.h"
#include "foundation/kernel_id.h"
#include "foundation/transition_stamp.h"
#include "kernel/command/command_canonical.h"
#include "kernel/command/command_table.h"
#include "kernel/config/kernel_config.h"
#include "kernel/config/store_limits.h"
#include "kernel/intent/intent.h"
#include "kernel/intent/transition_clock.h"
#include "work/agent/agent_table.h"
#include "work/authority/authority_cleanup.h"
#include "work/call/call_table.h"
#include "work/goal/goal_authority.h"
#include "work/goal/goal_table.h"
#include "work/propagation.h"
#include "work/task/task_authority.h"
#include "work/task/task_table.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::kernel {

// The Host monotonic clock, borrowed for one synchronous owner call. A rule
// takes the sample the kernel made at entry (now_us) for schedules and the
// checks before any caller code runs, and samples this clock again for a
// check after caller code returns (a goal type's export, a strategy's
// decide), so a deadline that passed meanwhile counts (ADR-0051).
class OwnerClock {
  public:
    OwnerClock(void *ctx, uint64_t (*read)(void *ctx)) : ctx_(ctx), now_us_(read) {}
    uint64_t now_us() const { return now_us_(ctx_); }

  private:
    void *ctx_;
    uint64_t (*now_us_)(void *ctx);
};

// The ingress frontier, borrowed for one synchronous admission. Only the
// publication of a new explicit reauthorization samples it, never a retry.
class InputResumeSampler {
  public:
    InputResumeSampler(void *ctx, work::InputResumeBoundary (*read)(void *ctx))
        : ctx_(ctx), sample_(read) {}
    work::InputResumeBoundary sample() const { return sample_(ctx_); } // Taken under its lock.

  private:
    void *ctx_;
    work::InputResumeBoundary (*sample_)(void *ctx);
};

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

// One stable scan identity; an orphaned dedicated Grant may retain a Goal ID
// whose table slot has been reused. That case has index == no_index.
struct AuthorityCleanupGoal {
    uint64_t goal_id = 0;                       // Exact nonzero selected ID; 0 before selection.
    uint32_t index = work::GoalTable::no_index; // Retained Goal index, or none for an orphan.
};

struct WorkModel {
    WorkModel(const KernelCapacities &capacities, const KernelLimits &limits,
              const KernelStorage &storage)
        : sessions(storage[Store::sessions], capacities.session_capacity),
          goal_types(storage[Store::goal_types], capacities.goal_type_capacity),
          agents(storage[Store::agents], capacities.agent_capacity),
          goals(storage[Store::goals],
                {capacities.goal_capacity, capacities.goal_parameter_max_bytes,
                 limits.goal_depth_max, limits.goal_children_max}),
          tasks(storage[Store::tasks], task_limits(capacities), limits.task_model_call_hard_limit),
          strategies(storage[Store::strategies], capacities.strategy_capacity),
          commands(storage[Store::commands], command_limits(capacities)),
          capabilities(storage[Store::capabilities], capacities.capability_capacity,
                       capacities.authority_scope_max_bytes),
          observation_sources(storage[Store::observation_sources],
                              capacities.observation_source_capacity),
          scenes(storage[Store::scenes], scene_limits(capacities), capabilities,
                 observation_sources),
          execution_catalog(storage[Store::execution_catalog], capacities.executor_capacity,
                            capacities.execution_binding_capacity),
          grants(storage[Store::grants], grant_limits(capacities)),
          goal_authority(storage[Store::goal_authority], goal_authority_limits(capacities)),
          task_authority(storage[Store::task_authority], task_authority_limits(capacities)),
          calls(storage[Store::calls], call_limits(capacities)) {
        tasks.set_retention({this, retains_task});
        goals.set_retention({this, retains_goal});
    }

    WorkModel(const WorkModel &) = delete;
    WorkModel &operator=(const WorkModel &) = delete;

    // Entering RUNNING: ids embed the durable boot epoch (C-ID-010), the
    // first transition takes first_stamp, and the instances of all agents
    // still REGISTERED start in that one transition when there are any
    // (C-LIFE-025); the tasks recovery restored settle against their goals
    // and owners in it too (work::settle_restored). Returns whether there
    // was that transition.
    bool start(uint64_t boot_epoch, foundation::TransitionStamp first_stamp);

    work::WorkTables tables() {
        return {goals, tasks, agents, goal_authority,
                grants.has_compatibility_holds() ? &grants : nullptr};
    }
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
    // Immutable parent ceiling coverage; no current permission substitution.
    bool parent_covers_authority(uint64_t parent_id, const work::GoalAuthorityRecord &record) const;
    // Parent work/expiry and authorization are checked under the same owner sample.
    authority::GrantCheck check_parent_authority(uint32_t parent_index, uint64_t now_us,
                                                 uint64_t durable_seq) const;
    // Cross-table identity relation, shared by consumption and recovery proofs.
    bool matches_task_ceiling(const work::TaskAuthorityRecord &record, uint32_t goal_index) const;
    // One preflight for runtime and replay, before replacing the Goal ceiling's selections.
    authority::GrantCheck check_reauthorization_tasks(uint32_t goal_index) const;
    // Check the Task's own admitted identities against its immutable Goal ceiling.
    authority::GrantCheck check_task_authority(uint32_t task_index, uint64_t now_us,
                                               uint64_t durable_seq) const;
    // Publish inherited selections after derivation; hand off any replaced Task references.
    bool inherit_task_authority(uint64_t task_id, foundation::TransitionStamp stamp);
    // Release a Task's selected references at one accepted transition boundary.
    void release_task_references(const work::TaskAuthorityRecord &record, uint64_t commit_seq);
    // Release and clear exact Task metadata before its terminal Goal gives way.
    void clear_goal_task_authority(uint32_t goal_index, uint64_t commit_seq);
    // Persist authorization pause for this Goal and its direct live Task admissions;
    // preserve their user/strategy state and saturated revisions.
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
    // Request a persisted stable-ID pass after dependency/terminal changes.
    void request_authority_cleanup(foundation::TransitionStamp stamp);
    // Whether changed Goal/Grant facts in this transition request another pass.
    bool authority_cleanup_change_in(foundation::TransitionStamp stamp) const;
    // Least retained Goal/dedicated-Grant identity in the fixed active range.
    // False preserves out; no policy or model mutation occurs here.
    bool next_authority_cleanup_goal(AuthorityCleanupGoal &out) const;
    // One Goal and its direct live Tasks, or an empty-range cursor closure.
    bool advance_authority_cleanup(uint64_t now_us, uint64_t durable_seq);
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
    catalog::ObservationSources
        observation_sources;  // Immutable types shared by Scenes and knowledge.
    authority::Scenes scenes; // Finite restrictions and owner preference; no live observations.
    catalog::ExecutionRegistry
        execution_catalog; // Trusted ports/limits, independent of live permits.
    authority::GrantTable grants;
    // Work-owned immutable ceilings and selected Grant identities; no live Grant cache.
    work::GoalAuthorityTable goal_authority;
    work::TaskAuthorityTable task_authority; // Independent historical selections; no Scope copy.
    work::CallTable calls; // Original call identities, canonical bodies and action facts.
    work::AuthorityCleanup authority_cleanup; // Work-owned active/pending scan, no arena needed.
    foundation::IdAllocator ids;
    TransitionClock transitions;
    // Union of local completed steps and accepted call keys; a failed call still
    // consumes its Task's bounded step capacity and cannot be disguised as local progress.
    uint32_t accepted_step_count(uint32_t task_index) const;
    bool step_taken(uint32_t task_index, ayni_string_v1 key) const;

    // Runs call, which calls caller code (a goal type's validator or
    // requirement export, a strategy's decide function, a Provider or an
    // executor port), with calling_out() true and returns call's result.
    // The value before the call is restored after it, so a nested callout
    // leaves the enclosing one marked (ADR-0051).
    template <typename Call> auto call_out(Call &&call) -> decltype(call()) {
        const bool enclosing = calling_out_;
        calling_out_ = true;
        const auto result = call();
        calling_out_ = enclosing;
        return result;
    }
    // True while caller code runs inside call_out. Caller code must not call
    // the kernel; as a fail-safe, owner-thread functions that change state
    // then refuse to run (ayni.h, C-POLICY-001), so nothing changes between a
    // check and the change it allows.
    bool calling_out() const { return calling_out_; }

    // The goals' next due work by now_us (work::find_due_work over these
    // tables). A search that finds none is kept, with when the next due work
    // comes, and answers again until a transition, another durable_seq, the
    // last id being taken, forget_due_work() or that time (ADR-0019,
    // ADR-0020, ADR-0022, ADR-0051). Found work is not kept: doing it is a
    // transition. out_next_us is now_us when work is found.
    bool next_due_work(uint64_t now_us, uint64_t durable_seq, work::DueWork &out_work,
                       uint64_t &out_next_us);
    // For a change to the tables outside a transition, which only recovery
    // makes: WorkReplay's reanchoring and holds, and CallRuntime's hold of a
    // restored call whose binding changed. Every transition forgets the
    // search by itself.
    void forget_due_work() { due_search_kept_ = false; }

  private:
    // The kept search and what it read besides the tables; the fields stay
    // apart so they pack with calling_out_.
    uint64_t due_search_commit_seq_ = 0; // transitions.next_commit_seq() at the search.
    uint64_t due_search_durable_seq_ = 0;
    uint64_t next_due_work_us_ = work::no_time;
    bool due_search_kept_ = false;
    bool due_search_with_id_ = true;
    bool calling_out_ = false;
    // Cross-table retention reads owner facts; no duplicated reference counters.
    static bool retains_task(const void *context, const work::Task &task);
    static bool retains_goal(const void *context, const work::Goal &goal);
};

// Opens for the unchanged registered Principal already checked by the ABI
// entry. AYNI_OK with a new id, or AYNI_CAPACITY_EXCEEDED when the session table
// is full or this boot has no id left; failure preserves out_session_id.
ayni_status_t open_session(WorkModel &model, const authority::Principal &principal,
                           ayni_channel_t channel, uint64_t &out_session_id);

// Steps 4 and 5 of ayni_submit_intent: the session, the command id, then
// room for a transition and the rules of the intent's kind; applies the
// intent as one transition if they pass, and maps its command. The kernel
// is RUNNING. failure is reset, then says why on a refusal.
ayni_status_t apply_intent(WorkModel &model, const Intent &intent, const IntentContext &context,
                           IntentOutcome &outcome, IntentFailure &failure);

// Records a local input from source as an occurrence, due at now_us, of
// every goal whose trigger takes it, all in one transition (ADR-0021);
// input is the ingress frontier the input leaves. Returns false, changing
// nothing, when no goal takes it.
bool apply_local_input(WorkModel &model, const catalog::CatalogId &source, uint64_t now_us,
                       work::InputResumeBoundary input);

// Does the goals' next due work by now_us (work::find_due_work): expires a
// goal, derives a task or skips occurrences, each as one transition.
// Returns none without mutation when nothing is due. An expiry that would
// exhaust a revision drops the goal's expiry instead (C-COMMIT-012).
// Explicitly distinguish no work from a publication invariant failure.
enum class DueWorkResult { none, applied, internal };
DueWorkResult do_due_work(WorkModel &model, uint64_t now_us, uint64_t authority_now_us,
                          uint64_t durable_seq);
// Whether do_due_work(now_us) would do something; out_next_us is then now_us.
// When it would not, sets out_next_us to when the next due work or Grant
// expiry comes (work::no_time: none will without a change). Scans the goal
// table, each goal along at most goal_depth_max ancestors, and the task
// table only when WorkModel::next_due_work keeps no search.
bool refresh_due_work(WorkModel &model, uint64_t now_us, uint64_t &out_next_us,
                      uint64_t durable_seq);

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_INTENT_WORK_MODEL_H
