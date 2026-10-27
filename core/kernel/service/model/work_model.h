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
#include "foundation/kernel_id.h"
#include "foundation/transition_stamp.h"
#include "kernel/config/catalogs.h"
#include "kernel/config/kernel_config.h"
#include "kernel/ports/callback_gate.h"
#include "kernel/service/command/command_canonical.h"
#include "kernel/service/command/command_table.h"
#include "kernel/service/model/context.h"
#include "kernel/service/model/store_limits.h"
#include "kernel/service/model/transition_clock.h"
#include "work/agent/agent_table.h"
#include "work/authority/authority_cleanup.h"
#include "work/call/call_table.h"
#include "work/goal/calendar_table.h"
#include "work/goal/goal_authority.h"
#include "work/goal/goal_table.h"
#include "work/privacy/privacy_cleanup.h"
#include "work/propagation.h"
#include "work/task/task_authority.h"
#include "work/task/task_table.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::kernel {

// One stable scan identity; an orphaned dedicated Grant may retain a Goal ID
// whose table slot has been reused. That case has index == no_index.
struct AuthorityCleanupGoal {
    uint64_t goal_id = 0;                       // Exact nonzero selected ID; 0 before selection.
    uint32_t index = work::GoalTable::no_index; // Retained Goal index, or none for an orphan.
};

struct WorkModel {
    // catalogs outlives the model.
    WorkModel(const Catalogs &registered, const KernelCapacities &capacities,
              const KernelLimits &limits, const KernelStorage &storage, CallbackGate &callbacks)
        : catalogs(registered), sessions(storage[Store::sessions], capacities.session_capacity),
          agents(storage[Store::agents], capacities.agent_capacity),
          goals(storage[Store::goals],
                {capacities.goal_capacity, capacities.goal_parameter_max_bytes,
                 limits.goal_depth_max, limits.goal_children_max}),
          calendars(storage[Store::calendars], capacities.goal_capacity),
          tasks(storage[Store::tasks], task_limits(capacities), limits.task_model_call_hard_limit),
          commands(storage[Store::commands], command_limits(capacities)),
          scenes(storage[Store::scenes], scene_limits(capacities), registered.capabilities,
                 registered.observation_sources),
          grants(storage[Store::grants], grant_limits(capacities)),
          goal_authority(storage[Store::goal_authority], goal_authority_limits(capacities)),
          task_authority(storage[Store::task_authority], task_authority_limits(capacities)),
          calls(storage[Store::calls], call_limits(capacities)),
          memories(storage[Store::memories], memory_limits(capacities)),
          contexts(storage[Store::contexts], context_capacity(capacities)),
          provenance(storage[Store::provenance], provenance_limits(capacities)),
          callbacks_(callbacks) {
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
        return {goals,
                tasks,
                agents,
                goal_authority,
                grants.has_compatibility_holds() ? &grants : nullptr,
                &pause_groups};
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
    bool inherit_task_metadata(uint64_t task_id, foundation::TransitionStamp stamp);
    // Release a Task's selected references at one accepted transition boundary.
    void release_task_references(const work::TaskAuthorityRecord &record, uint64_t commit_seq);
    // Release and clear exact Task metadata before its terminal Goal gives way.
    void retire_goal_task_metadata(uint32_t goal_index, uint64_t commit_seq);
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
    // Content owners release selected Grants with the same transition as source replacement.
    void release_content_references(const knowledge::MemoryDependencies &dependencies,
                                    uint64_t commit_seq);
    // Stable work slot replacement unlinks its old content sources, then clears the slot.
    void clear_content_provenance(work::ProvenanceKind kind, uint32_t slot, uint64_t id,
                                  uint64_t commit_seq);
    // Current and invalid retained content both protect their captured Grant identities.
    // Fence the next command eviction before replacing its captured Grant references.
    void release_command_sources(uint64_t principal_id, uint64_t commit_seq);
    void protect_content_grants(authority::GrantReclaimExclusions &excluded) const;
    // Hand off removed references after all fallible checks. Repeated releases
    // retain the latest fence; no Goal/Grant reference count is duplicated.
    void release_goal_references(const work::GoalAuthorityRecord &record, uint64_t commit_seq);
    // A replaced Goal slot retires exactly its former calendar, authority and source metadata.
    void retire_goal_metadata(uint32_t index, uint64_t previous_id, uint64_t commit_seq);
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

    // Goal types, strategies, capability schemas, observation sources and
    // executors the rules read; closed before the model starts.
    const Catalogs &catalogs;
    // Registration fills principals, agents and Scene declarations in
    // CONFIGURING; the rest changes only in RUNNING.
    authority::PrincipalRegistry principals;
    authority::SessionTable sessions;
    work::AgentTable agents;
    work::GoalTable goals;
    work::PauseGroups pause_groups; // Independent bounded generic gates, never user lifecycle.
    work::CalendarTable calendars;  // Sole owner of civil rules, stable cursors and sampled UTC.
    work::TaskTable tasks;
    // The commands accepted, which the log keeps with their transitions.
    CommandTable commands;
    authority::Scenes scenes; // Finite restrictions and owner preference; no live observations.
    authority::GrantTable grants;
    // Work-owned immutable ceilings and selected Grant identities; no live Grant cache.
    work::GoalAuthorityTable goal_authority;
    work::TaskAuthorityTable task_authority; // Independent historical selections; no Scope copy.
    work::CallTable calls;           // Original call identities, canonical bodies and action facts.
    knowledge::MemoryTable memories; // Sole owner of memory facts, bodies and provenance.
    knowledge::ContextTable
        contexts; // File publication barriers and complete sources; no file text.
    work::PrivacyCleanup
        privacy; // Persistent privacy progress; no bodies, callbacks or storage handles.
    work::ProvenanceTable provenance; // Work-owned complete content dependencies, no body copies.
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
        return callbacks_.invoke(call);
    }
    // True while caller code runs inside call_out. Caller code must not call
    // the kernel; as a fail-safe, owner-thread functions that change state
    // then refuse to run (ayni.h, C-POLICY-001), so nothing changes between a
    // check and the change it allows.
    bool calling_out() const { return callbacks_.active(); }

    // Refresh one sampled UTC frontier and all civil projections; no transition.
    void refresh_calendars(work::CalendarClock clock);
    // The goals' next due work by now_us (work::find_due_work over these
    // tables). A search that finds none is kept, with when the next due work
    // comes, and answers again until a transition, another durable_seq, the
    // last id being taken, forget_due_work() or that time (ADR-0019,
    // ADR-0020, ADR-0022, ADR-0051). Found work is not kept: doing it is a
    // transition. out_next_us is now_us when work is found.
    bool next_due_work(uint64_t now_us, uint64_t durable_seq, work::DueWork &out_work,
                       uint64_t &out_next_us);
    // For a change outside a transition to what the search reads. Recovery
    // changes the tables: WorkReplay's reanchoring and holds, and
    // CallRuntime's hold of a restored call whose binding changed. The task
    // table also reads whether the runtime still holds a call's slot or
    // lease (CallRuntime::runtime_retains), which a stopped call gives up
    // without a transition. Every transition forgets the search by itself.
    void forget_due_work() { due_search_kept_ = false; }

  private:
    // The kept search and what it read besides the tables. The other four
    // fields mean something only while due_search_kept_ is set. Recovery or
    // changes outside a transition explicitly invalidate this cache.
    uint64_t due_search_commit_seq_ = 0;        // transitions.next_commit_seq() at the search.
    uint64_t due_search_durable_seq_ = 0;       // The durable_seq it was given.
    uint64_t next_due_work_us_ = work::no_time; // The next due time it found, or no_time.
    bool due_search_kept_ = false;              // Whether a search that found none is kept.
    bool due_search_with_id_ = true;            // Whether an id was left at the search.
    CallbackGate &callbacks_; // Borrowed Kernel boundary; outlives all model callbacks.
    // Cross-table retention reads owner facts; no duplicated reference counters.
    static bool retains_task(const void *context, const work::Task &task);
    static bool retains_goal(const void *context, const work::Goal &goal);
};

// Current durable owner binding and authenticated management channel.
bool bound_management_session(const WorkModel &model, const authority::Session &session,
                              uint64_t durable_seq);

// Opens for the unchanged registered Principal already checked by the ABI
// entry. AYNI_OK with a new id, or AYNI_CAPACITY_EXCEEDED when the session table
// is full or this boot has no id left; failure preserves out_session_id.
ayni_status_t open_session(WorkModel &model, const authority::Principal &principal,
                           ayni_channel_t channel, uint64_t &out_session_id);

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
