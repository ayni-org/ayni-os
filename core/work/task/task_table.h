// Tasks (contracts/domain/lifecycles.md Task, ADR-0018, ADR-0022, ADR-0024).
// The table owns each task's lifecycle, revision, hold, place, steps and
// private checkpoint. A task belongs to one goal and keeps that goal's owner
// for its life (C-LIFE-020). A READY task waits for its strategy's decision,
// which ends it, holds it, or commits steps and leaves it READY. A task's
// steps are unique, bounded and only ever added to. Subscription waits may
// resume without adding a step; each wake requires new sample/state evidence.
//
// An ended task keeps its place until a new task needs one: then the task
// that ended first gives way, and a task that has not ended never does. A
// goal that gives way takes its tasks, which have all ended, with it
// (ADR-0025). A place given up moves another task into it, so tasks are
// found by id and an index is valid only until the next insert() or
// remove_goal(). Steps and checkpoints lie in blocks of their own, one per
// slot, and a task keeps its slot for its life: a new task takes the slot
// of the task it replaces, or one no task holds.
//
// Recovery rebuilds the table from the log (ADR-0026): restore() and the
// restore_* functions replay what each committed transition changed, and
// hold_restored() and reanchor() then fit the tasks to this boot.

#ifndef AYNI_CORE_WORK_TASK_TASK_TABLE_H
#define AYNI_CORE_WORK_TASK_TASK_TABLE_H

#include "catalog/catalog_id.h"
#include "foundation/bounded_array.h"
#include "foundation/kernel_id.h"
#include "foundation/transition_stamp.h"
#include "work/task/operation_wait.h"

#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ayni::work {

// A task the kernel derives from occurrences of a goal.
struct NewTask {
    uint64_t goal_id;
    uint64_t owner_instance_id; // the goal's
    uint64_t due_us;            // when its latest occurrence fell due
    uint32_t goal_index;        // in the goal table
    uint32_t occurrence;        // the number of the latest occurrence it stands for
    uint32_t occurrence_count;  // how many it stands for, at least 1
    int64_t calendar_local_s = AYNI_CALENDAR_TIME_NONE; // Planned civil key; -1 for other triggers.
    int64_t calendar_utc_s = AYNI_CALENDAR_TIME_NONE;   // Resolved UTC; -1 iff no civil key.
};

// Durable per-Task model admission facts (C-RUN-002/003). Request acceptance
// owns the increment; a failed next admission owns the hold. No cost balance.
struct TaskModelCalls {
    uint32_t used = 0; // Accepted requests, <= limit; never decremented.
    uint32_t limit = AYNI_TASK_MODEL_CALL_LIMIT_DEFAULT; // Positive soft ceiling.
    bool blocked = false; // True only when used == limit and a next request was refused.
    // Complete equality for immutable authorization transitions and codecs.
    bool operator==(const TaskModelCalls &other) const {
        return used == other.used && limit == other.limit && blocked == other.blocked;
    }
    // Opposite of complete equality; no independent comparison formula.
    bool operator!=(const TaskModelCalls &other) const { return !(*this == other); }
    // One validator for struct/record inputs, independent of this boot's ceiling.
    bool valid() const {
        return limit > 0U && limit <= AYNI_TASK_MODEL_CALL_HARD_LIMIT_MAX && used <= limit &&
               (!blocked || used == limit);
    }
};

// The Task owns its selected call reference, while work/call owns call facts.
// Cancellation is a requested end, never proof that the borrowed attempt stopped.
struct TaskCall {
    enum class End : uint32_t { none = 0, cancel, expire };
    uint64_t id = 0;     // Zero iff no logical call blocks this Task's next decision.
    End end = End::none; // Requested end after drainage; none while ordinary work waits.
    // One shape rule used by live state and record readers.
    bool valid(ayni_task_lifecycle_t lifecycle) const {
        if (id == 0U) {
            return end == End::none && lifecycle != AYNI_TASK_RUNNING &&
                   lifecycle != AYNI_TASK_CANCELLING;
        }
        return foundation::is_kernel_id(id) &&
               ((end == End::none &&
                 (lifecycle == AYNI_TASK_RUNNING || lifecycle == AYNI_TASK_PAUSED)) ||
                ((end == End::cancel || end == End::expire) && lifecycle == AYNI_TASK_CANCELLING));
    }
    // Complete relation equality for metadata-only authorization changes.
    bool operator==(const TaskCall &other) const { return id == other.id && end == other.end; }
    bool operator!=(const TaskCall &other) const { return !(*this == other); }
};

struct Task {
    uint64_t task_id;
    uint64_t goal_id;
    uint64_t owner_instance_id;
    uint64_t replaced_task_id; // the ended task whose place it took; 0: none
    uint64_t created_transition_id;
    uint64_t commit_seq; // of the last change
    // The commit_seq of the last transition that added steps, and of the
    // last that wrote the private checkpoint; 0: none has.
    uint64_t steps_seq;
    uint64_t checkpoint_seq;
    uint64_t due_us;
    uint32_t goal_index;
    uint32_t occurrence;
    uint32_t occurrence_count;
    int64_t calendar_local_s = AYNI_CALENDAR_TIME_NONE; // Planned civil key; -1 for non-calendar.
    int64_t calendar_utc_s = AYNI_CALENDAR_TIME_NONE; // Resolved planned UTC; -1 iff no civil key.
    uint32_t revision;                                // 1 at creation, raised by each change
    uint32_t cancel_revision;                         // the revision that cancelled it; 0 otherwise
    ayni_task_lifecycle_t lifecycle;
    // Policy failure or pending-call hold overlay; AYNI_OK/REASON_NONE means none.
    // UNKNOWN and RESOURCE_WAIT preserve the independent user lifecycle; their
    // owner transition clears them. Other diagnostics remain after termination.
    ayni_status_t hold_status;
    ayni_reason_t hold_reason;
    // What its strategy's decisions committed (ADR-0024): how many steps,
    // how many of them there were before steps_seq, and how large a private
    // checkpoint which version of which strategy wrote; version 0 and an
    // empty id before the first. Kept once the task ends.
    uint32_t step_count;
    uint32_t steps_before;
    uint32_t checkpoint_size;
    uint32_t checkpoint_writer_version;
    catalog::CatalogId checkpoint_writer_id;
    bool call_unknown =
        false; // Task-owned independent hold; only same-batch Call propagation changes it.
    TaskModelCalls model_calls{}; // This Task owns its consumption and independent durable hold.
    TaskCall call{}; // Persisted selected logical call and requested end, never a mailbox slot.
    std::optional<OperationWait>
        operation_wait{}; // Present only in WAITING or its user PAUSED overlay.
    uint32_t slot;        // the table's own: which blocks hold them
    // This boot cannot serve an otherwise recoverable task. Kept apart
    // from the durable state so compaction cannot make the hold permanent.
    ayni_status_t recovery_hold_status;
    ayni_reason_t recovery_hold_reason;
};

// What a TASK_STATE record (ADR-0026) holds.
struct TaskState {
    ayni_task_lifecycle_t lifecycle;
    uint32_t revision;
    uint32_t cancel_revision;
    ayni_status_t hold_status;
    ayni_reason_t hold_reason;
    uint32_t step_count;
    TaskModelCalls model_calls{}; // Preserved across lifecycle/authorization changes.
    TaskCall call{}; // Preserved across authorization; only the call coordinator may clear it.
    std::optional<OperationWait>
        operation_wait{}; // Present only in WAITING or its user PAUSED overlay.
    bool call_unknown =
        false; // Task-owned independent hold; only same-batch Call propagation changes it.
};

// A wait owns no foreground call. User pause preserves its exact frontier.
inline bool valid_operation_wait(const std::optional<OperationWait> &wait,
                                 ayni_task_lifecycle_t lifecycle, uint64_t call_id) {
    return wait ? wait->valid() && call_id == 0 &&
                      (lifecycle == AYNI_TASK_WAITING || lifecycle == AYNI_TASK_PAUSED)
                : lifecycle != AYNI_TASK_WAITING;
}

// Pause takes READY to PAUSED and resume takes it back, unless a decision
// holds it; cancel takes a READY or PAUSED task to CANCELLED, and expire,
// which comes only with its goal's expiry (ADR-0020), to EXPIRED. Complete
// and fail come from its strategy's decision and take a READY task to
// COMPLETED and FAILED (ADR-0022); a decision may also hold the task
// (hold()) or leave it READY (progress()). A selected call makes it RUNNING;
// cancellation/expiry keeps it CANCELLING until the call has a known outcome.
enum class TaskChange { pause, resume, cancel, expire, complete, fail };

class TaskTable {
  public:
    // Resource hold/release, UNKNOWN hold/release and final end may still be required after detach.
    static constexpr uint32_t background_call_revisions = 5;
    static constexpr uint32_t no_index = 0xFFFFFFFFU; // no task's index

    struct Limits {
        uint32_t capacity;             // at most AYNI_TASK_CAPACITY_MAX
        uint32_t step_capacity;        // per task, at most AYNI_TASK_STEP_CAPACITY_MAX
        uint32_t checkpoint_max_bytes; // per task, at most AYNI_TASK_CHECKPOINT_MAX_BYTES_MAX
    };

    // Why a change is refused, in the order check() checks.
    enum class ChangeCheck {
        allowed,
        revision_conflict,
        invalid_state,
        revision_exhausted,
    };

    // How a record replayed: restored; corrupt when no transition this
    // library commits writes it in this state; no_room when the limits of
    // this boot cannot hold it. Neither of the last two changes anything.
    enum class Restore { restored, corrupt, no_room };

    // The tasks, then step_capacity steps per slot, then a private
    // checkpoint of checkpoint_max_bytes per slot.
    static constexpr std::size_t storage_alignment =
        foundation::BoundedArray<Task>::storage_alignment;
    static std::size_t storage_size(const Limits &limits);

    // storage holds storage_size(limits) bytes aligned to storage_alignment
    // and outlives the table. model_call_hard_limit (1..64 calls) bounds each
    // Task's model requests and takes no storage, so it is not a Limit.
    TaskTable(void *storage, const Limits &limits,
              uint32_t model_call_hard_limit = AYNI_TASK_MODEL_CALL_HARD_LIMIT_DEFAULT);

    TaskTable(const TaskTable &) = delete;
    TaskTable &operator=(const TaskTable &) = delete;

    // Borrowed immutable relation query, installed by the work coordinator.
    // A terminal Task may still own unresolved calls or retained dependent data.
    struct Retention {
        const void *context = nullptr; // Coordinator outlives this table; null when disabled.
        bool (*retains)(const void *, const Task &) = nullptr; // Pure owner-thread query, no I/O.
    };
    // Owner projection, never cached: background Calls reserve UNKNOWN hold/release writes.
    struct CallConstraints {
        uint32_t revisions =
            0;                // Additional mandatory writes; zero with no pending background Call.
        bool queued = false;  // True iff an actual Task-owned Call has not dispatched.
        bool unknown = false; // True while any original execution remains ABANDONED.
    };
    using CallQuery = CallConstraints (*)(const void *, const Task &);
    void set_call_query(const void *context, CallQuery query) {
        call_context_ = context;
        call_query_ = query;
    }
    // Include cross-Call mandatory writes in ordinary mutation admission.
    bool has_revisions(const Task &task, uint32_t required, uint32_t prospective_reserve = 0) const;
    bool at_revision_limit(const Task &task) const;
    // Clear independent UNKNOWN/resource overlays only when the Call owner proves no source
    // remains.
    void settle_call_holds(uint32_t index, foundation::TransitionStamp stamp);
    void set_retention(Retention retention) { retention_ = retention; } // Before live work starts.

    // Whether insert() would succeed: a free place, or an ended task to give
    // way.
    bool has_room() const;

    // Creates a READY task at revision 1, giving it the place of the task
    // that ended first, by commit_seq and then task id, if the table is
    // full. Returns false, changing nothing, when has_room() is false.
    bool insert(const NewTask &task, uint64_t task_id, foundation::TransitionStamp stamp);
    // Removes the tasks of goal goal_index as the goal gives way
    // (GoalTable::giving_way()). They have all ended: a goal with a task that
    // has not ended does not give way.
    void remove_goal(uint32_t goal_index);

    // The checks of a change a caller asked for by revision (TASK_CANCEL).
    ChangeCheck check(uint32_t index, TaskChange change, uint32_t expected_revision) const;
    // Only when allows(at(index), change) and the revision is below the
    // largest. The task gets the next revision and stamp.commit_seq.
    void apply(uint32_t index, TaskChange change, foundation::TransitionStamp stamp);
    // Autonomous authorization metadata changes share the Task's log stamp,
    // while user lifecycle, revision, strategy hold and progress stay intact.
    void authority_changed(uint32_t index, foundation::TransitionStamp stamp);
    // Caller preflighted a direct live cohort with non-saturated revisions.
    // Preserve all user/policy/compatibility state and private progress;
    // the separate TaskAuthorityTable owns the replacement authorization.
    void reauthorize(uint32_t index, foundation::TransitionStamp stamp);

    // Owner management preflights CAS, live state and a strictly higher soft
    // limit within this boot's hard ceiling; refusal changes no Task state.
    ayni_status_t check_model_limit_raise(uint32_t index, uint32_t expected_revision,
                                          uint32_t limit) const;
    // Only after check_model_limit_raise succeeds, in the command transition.
    // Preserves count/progress and all other holds, advancing revision once.
    void raise_model_limit(uint32_t index, uint32_t limit, foundation::TransitionStamp stamp);
    // Positive startup count ceiling; not a concurrent-slot allocation.
    uint32_t model_call_hard_limit() const { return model_call_hard_limit_; }
    // Internal MA-008 admission boundary: caller atomically commits the same
    // accepted call identity; only with READY, spare count and revision capacity.
    void accept_model_call(uint32_t index, foundation::TransitionStamp stamp);
    // Internal admission refusal with used == limit; prevents further decisions
    // until an owner raises it. Does not overwrite user/policy/recovery state.
    void hold_model_calls(uint32_t index, foundation::TransitionStamp stamp);

    // Kind controls only whether acceptance consumes the independent model counter.
    enum class CallKind { tool, model };
    // Caller preflighted READY, spare step/count/revision and a valid new call ID;
    // the same B2 transition owns its Call record, checkpoint and resource intent.
    void begin_call(uint32_t index, uint64_t call_id, CallKind kind,
                    foundation::TransitionStamp stamp);
    // Exact selected call is drained and either known or the Task's end was
    // requested. Preserve user pause; cancellation/expiry never reopens decisions.
    // Unknown call records remain pinned independently by the retention query.
    void finish_call(uint32_t index, foundation::TransitionStamp stamp);
    // Expire one queued background Task; caller pairs a durable SKIPPED Call fact.
    void skip_queued_call(uint32_t index, foundation::TransitionStamp stamp);
    // Resource wait expired before dispatch: retain the selected call and user
    // lifecycle, adding only an explicit-resume overlay (ADR-0045).
    void pause_resource_wait(uint32_t index, foundation::TransitionStamp stamp);
    // Exact Task CAS and revision reserve; does not consume execution authorization.
    ayni_status_t check_resume_wait(uint32_t index, uint32_t expected_revision) const;
    // Prechecked owner command removes only the wait overlay, preserving user pause.
    void resume_wait(uint32_t index, foundation::TransitionStamp stamp);
    // Journal-only v12, paired with the exact owner management command.
    Restore restore_wait_resume(uint32_t index, const TaskState &state,
                                foundation::TransitionStamp stamp);
    // This diagnostic pair denotes the durable wait overlay; no queue owns it.
    static bool waiting_for_resource(const Task &task) {
        return task.hold_status == AYNI_EXPIRED && task.hold_reason == AYNI_REASON_RESOURCE_WAIT;
    }
    // Unknown remote outcome blocks decisions without losing the original user
    // pause state. Only reconciliation or explicit cancellation may change this hold.
    void hold_unknown_call(uint32_t index, foundation::TransitionStamp stamp);

    // Its strategy's decision holds the READY task index, below the largest
    // revision: it becomes PAUSED and no change but an end takes it out.
    // It gets the next revision and stamp.commit_seq. status is a failure.
    void hold(uint32_t index, ayni_status_t status, ayni_reason_t reason,
              foundation::TransitionStamp stamp);

    // Reserve wait, wake and terminal revisions before entering an event wait.
    static bool can_wait_operation(const Task &task);
    // Prevalidated READY decision retains its checkpoint without taking another step key.
    void wait_operation(uint32_t index, const OperationWait &basis,
                        foundation::TransitionStamp stamp);
    // Latest data/state changed; ready exactly once, with a final revision reserved for
    // cancellation.
    bool wake_operation(uint32_t index, foundation::TransitionStamp stamp);
    // The steps and private checkpoint a decision of the READY task index
    // commits, in the transition stamp that then applies the decision by
    // progress() or work::end_task() (ADR-0024). add_step appends key and
    // returns true when it is a catalog id that is not yet a step of the
    // task and the task has fewer than step_capacity() steps; otherwise it
    // returns false, changing nothing. set_checkpoint: the strategy
    // writer_id at writer_version, nonzero, wrote state, at most
    // checkpoint_max_bytes(), which replaces the task's private checkpoint.
    bool add_step(uint32_t index, ayni_string_v1 key, foundation::TransitionStamp stamp);
    void set_checkpoint(uint32_t index, const catalog::CatalogId &writer_id,
                        uint32_t writer_version, ayni_bytes_v1 state,
                        foundation::TransitionStamp stamp);
    // A decision of the READY task index, below the largest revision, that
    // neither ends nor holds it: it stays READY with the next revision and
    // stamp.commit_seq, so next_ready() takes the tasks READY before it
    // first.
    void progress(uint32_t index, foundation::TransitionStamp stamp);

    uint32_t step_capacity() const { return step_capacity_; }
    uint32_t checkpoint_max_bytes() const { return checkpoint_max_bytes_; }
    // The step < at(index).step_count of task index, in the order the
    // steps were committed.
    const catalog::CatalogId &step(uint32_t index, uint32_t step) const;
    bool has_step(uint32_t index, ayni_string_v1 key) const;
    // Valid until the task's next change.
    ayni_bytes_v1 checkpoint(uint32_t index) const;
    // Erase bytes under a separate provenance privacy stamp; keep the true workflow/writer facts.
    void erase_checkpoint(uint32_t index);

    static bool allows(const Task &task, TaskChange change);
    static bool at_largest_revision(const Task &task);
    // Reserve acceptance, UNKNOWN hold, cancellation and final drain revisions.
    // These bounds prevent user changes from consuming completion's last revision.
    static bool can_begin_call(const Task &task);
    // Include live background Call reserves without copying Call state into Task.
    bool can_begin_with_calls(const Task &task) const;
    // Admission also reserves future detach writes before the new background Call exists.
    bool can_begin_background_call(const Task &task) const;
    bool can_wait_with_calls(const Task &task) const;
    // Diagnostic precedence: policy, compatibility, then model-limit hold.
    // still_held() only counts live tasks; terminal diagnostics remain visible.
    static ayni_status_t effective_hold_status(const Task &task) {
        return task.hold_status != AYNI_OK            ? task.hold_status
               : task.recovery_hold_status != AYNI_OK ? task.recovery_hold_status
               : task.call_unknown                    ? AYNI_RESULT_UNKNOWN
               : task.model_calls.blocked             ? AYNI_CAPACITY_EXCEEDED
                                                      : AYNI_OK;
    }
    static ayni_reason_t effective_hold_reason(const Task &task) {
        return task.hold_status != AYNI_OK            ? task.hold_reason
               : task.recovery_hold_status != AYNI_OK ? task.recovery_hold_reason
               : task.call_unknown                    ? AYNI_REASON_NONE
               : task.model_calls.blocked             ? AYNI_REASON_TASK_MODEL_CALL_LIMIT
                                                      : AYNI_REASON_NONE;
    }
    static ayni_task_lifecycle_t effective_lifecycle(const Task &task) {
        return is_live(task.lifecycle) && task.lifecycle != AYNI_TASK_CANCELLING &&
                       (task.hold_status != AYNI_OK || task.recovery_hold_status != AYNI_OK ||
                        task.model_calls.blocked || task.call_unknown)
                   ? AYNI_TASK_PAUSED
                   : task.lifecycle;
    }
    static bool is_held(const Task &task) { return effective_hold_status(task) != AYNI_OK; }
    static bool still_held(const Task &task) {
        return effective_lifecycle(task) == AYNI_TASK_PAUSED && is_held(task);
    }

    // The task that has been READY longest among those READY since before
    // commit_seq ready_before, by commit_seq and then task id; a task at
    // the largest revision can take no decision and is left out. Returns
    // false when there is none.
    // Optional owner-provided admission filter; null accepts every candidate.
    // Both context and callback are borrowed for this non-reentrant query.
    struct ReadyFilter {
        const void *context; // Borrowed owner state; null is valid for stateless filters.
        bool (*accepts)(const void *, const Task &); // Pure selection; null accepts all candidates.
    };
    bool next_ready(uint64_t ready_before, uint32_t &out_index, ReadyFilter filter = {}) const;
    // Whether some task below the largest revision is READY.
    bool has_ready(ReadyFilter filter = {}) const;
    // Whether a task of goal goal_index other than task_index has not ended;
    // with no_index, whether any of its tasks has not ended.
    bool live_task_besides(uint32_t goal_index, uint32_t task_index) const;
    // Whether some task is still_held(), without looking at the tasks.
    bool has_held() const { return held_count_ > 0U; }

    // Returns false when no task in memory has the id.
    bool find(uint64_t task_id, uint32_t &out_index) const;

    // Replays a TASK record: the task as insert() created it in stamp, of
    // goal task.goal_index, in the place of replaced_task_id, an ended task,
    // or in a free place when that is 0. due_us is set by reanchor().
    Restore restore(const NewTask &task, uint64_t task_id, uint64_t replaced_task_id,
                    foundation::TransitionStamp stamp);
    // Imports a retained task, whose replaced predecessor need no longer
    // exist. Steps, checkpoint and state follow in snapshot order.
    Restore restore_snapshot(const NewTask &task, uint64_t task_id, uint64_t replaced_task_id,
                             foundation::TransitionStamp stamp, uint64_t steps_seq,
                             uint64_t checkpoint_seq, uint32_t steps_before);
    // Replays a TASK_STATE record of task index, after the steps of the same
    // transition: state.step_count is the task's.
    Restore restore_state(uint32_t index, const TaskState &state,
                          foundation::TransitionStamp stamp);
    // Metadata-only authorization updates preserve every user/policy field,
    // including a held Task. The paired authority fact proves the deny change.
    Restore restore_authority_state(uint32_t index, const TaskState &state,
                                    foundation::TransitionStamp stamp);
    // A direct live Task keeps user/policy state and progress, advancing its
    // revision exactly once for the new binding; terminal history is immutable.
    Restore restore_reauthorization_state(uint32_t index, const TaskState &state,
                                          foundation::TransitionStamp stamp);
    // A required runtime state paired with the owner's exact raise command;
    // no progress, lifecycle or other hold may change with it.
    Restore restore_model_limit_raise(uint32_t index, const TaskState &state,
                                      foundation::TransitionStamp stamp);
    // Replays one key of a TASK_STEPS record of task index, the step at
    // position step.
    Restore restore_step(uint32_t index, uint32_t step, ayni_string_v1 key,
                         foundation::TransitionStamp stamp);
    // Replays a TASK_CHECKPOINT record of task index.
    Restore restore_checkpoint(uint32_t index, ayni_string_v1 writer_id, uint32_t writer_version,
                               ayni_bytes_v1 state, foundation::TransitionStamp stamp);
    // Holds the live task index, unless a decision already holds it, for
    // what this boot lacks to decide it (ADR-0026): it becomes PAUSED at
    // its revision and commit_seq, so what the log holds is unchanged and a
    // later boot that has what was lacking finds the task as it was. status
    // is a failure.
    void hold_restored(uint32_t index, ayni_status_t status, ayni_reason_t reason);
    // Every task's latest occurrence falls due at now_us, the time recovery
    // ended (ADR-0026): a time before this boot is not on its clock.
    void reanchor(uint64_t now_us);
    // Empties the table, as a recovery that failed leaves it.
    void clear();

    // index < size().
    const Task &at(uint32_t index) const { return tasks_[index]; }
    uint32_t size() const { return tasks_.size(); }

    // Not ended: READY, RUNNING, PAUSED or CANCELLING.
    static bool is_live(ayni_task_lifecycle_t lifecycle);

  private:
    // The lowest slot no task holds; only when the table is not full.
    uint32_t free_slot() const;
    // Clear only Call-owned holds whose last source has ended; no revision/count change here.
    bool clear_settled_call_holds(Task &task);
    // The ended task that gives way to the next insert(): the one that
    // ended first, by commit_seq and then task id; no_index when none has.
    uint32_t giving_way() const;
    // Adds the new task in slot, after the caller made room.
    void place(const NewTask &task, uint64_t task_id, uint64_t replaced_task_id, uint32_t slot,
               foundation::TransitionStamp stamp);
    catalog::CatalogId *steps_of(uint32_t index) const;

    foundation::BoundedArray<Task> tasks_;
    catalog::CatalogId *steps_;
    uint8_t *checkpoints_;
    uint32_t step_capacity_;
    uint32_t checkpoint_max_bytes_;
    uint32_t model_call_hard_limit_;     // Fixed this boot; independent of steps and concurrency.
    uint32_t held_count_ = 0;            // still_held() tasks
    const void *call_context_ = nullptr; // Borrowed coordinator for the table lifetime.
    CallQuery call_query_ = nullptr;     // Pure owner-thread projection; null in standalone tables.
    Retention retention_{};              // External references, not a cached call/knowledge count.
};

} // namespace ayni::work

#endif // AYNI_CORE_WORK_TASK_TASK_TABLE_H
