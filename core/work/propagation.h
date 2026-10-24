// How changes to goals and instances reach their tasks, how a task's end or
// a skip reaches its goal, and which of the goals' due work comes next
// (ADR-0018, ADR-0020, ADR-0022, ADR-0023). The goal, task and agent tables
// each keep their own invariants; the rule here spans them: a task may
// advance only while its goal and all the goal's ancestors are ACTIVE and
// its owner instance is RUNNING. A change re-settles every READY or PAUSED
// task it reaches against that rule in the same transition, except a task
// its strategy's decision holds, and a goal's end takes the tasks it reaches
// with it. A goal that gives way to a new one takes its ended tasks out of
// the task table (ADR-0025). Authorization denies consumption independently
// and never rewrites user Task lifecycle during settlement (ADR-0038).
//
// The kernel calls these inside one transition on the owner thread: first
// the exhausts check, then apply_* or end_task, which use the same reach
// and the same rule, so a change that passes the check never stops halfway.

#ifndef AYNI_CORE_WORK_PROPAGATION_H
#define AYNI_CORE_WORK_PROPAGATION_H

#include "foundation/transition_stamp.h"
#include "work/agent/agent_table.h"
#include "work/goal/goal_authority.h"
#include "work/goal/goal_table.h"
#include "work/task/task_table.h"

#include <cstdint>

namespace ayni::work {

struct WorkTables {
    GoalTable &goals;
    TaskTable &tasks;
    AgentTable &agents;
    // Optional for standalone work-table users; Kernel always supplies its owner.
    const GoalAuthorityTable *authority = nullptr;
    // Borrowed durable-prefix sample; max means all supplied facts are durable.
    uint64_t durable_seq = UINT64_MAX;
};

// Whether a task the change would reach is at the largest revision. The goal
// table checks the goals themselves (GoalTable::check).
bool goal_change_exhausts_tasks(const WorkTables &tables, uint32_t goal_index, GoalChange change);
// Only after GoalTable::check allowed the change and
// goal_change_exhausts_tasks returned false.
void apply_goal_change(WorkTables &tables, uint32_t goal_index, GoalChange change,
                       foundation::TransitionStamp stamp);

// Creates the goal (GoalTable::insert). The goal that gives way to it, if
// any, takes its tasks out of the task table with it: they have all ended,
// since a goal's end takes the tasks it reaches with it and no task is
// derived for a terminal goal (ADR-0025). Returns false, changing nothing,
// when the goal table has no room.
bool insert_goal(WorkTables &tables, const NewGoal &goal, uint64_t goal_id,
                 foundation::TransitionStamp stamp, uint32_t &out_index);

// Whether a task, or for stop a goal, the change would reach is at the
// largest revision. The agent table checks the instance itself.
bool instance_change_exhausts(const WorkTables &tables, uint32_t instance_index,
                              InstanceChange change);
// Only after AgentTable::check allowed the change and
// instance_change_exhausts returned false. Stop cancels every goal and task
// the instance owns that is not yet terminal.
void apply_instance_change(WorkTables &tables, uint32_t instance_index, InstanceChange change,
                           foundation::TransitionStamp stamp);

// Whether ending the task task_index, below the largest revision, by change
// would raise a revision past the largest: when its goal ends with it, that
// of the goal, of a descendant or of a task the goal's end reaches. change
// is complete or fail for a READY task (its strategy's decision; a task at
// the largest revision is never decided, TaskTable::next_ready) or cancel
// for one TaskTable::check allowed to be cancelled (TASK_CANCEL).
bool task_end_exhausts(const WorkTables &tables, uint32_t task_index, TaskChange change);
// Only after task_end_exhausts returned false. Ends the task. If its goal
// then has no task that has not ended and no occurrence to come, the goal
// ends too, SATISFIED for complete, FAILED for fail and CANCELLED for
// cancel, and its descendants that are not terminal are cancelled with
// their tasks (ADR-0022, ADR-0023).
void end_task(WorkTables &tables, uint32_t task_index, TaskChange change,
              foundation::TransitionStamp stamp);

// Whether ending goal_index by change, satisfy or fail while it is ACTIVE,
// cancel or expire while it is not terminal, would raise a revision past the
// largest: the goal's own, a descendant's or that of a task the end reaches.
// Nothing lowers a revision, so such a goal never could end that way.
bool goal_end_exhausts(const WorkTables &tables, uint32_t goal_index, GoalChange change);

// Whether entering RUNNING settles a task recovery restored (ADR-0026): a
// READY or PAUSED task below the largest revision, not held, that the rule
// above takes to the other of the two, as when its owner's agent is no
// longer registered or is again.
bool settles_restored(const WorkTables &tables);
// Settles each of them in stamp, as a change does.
void settle_restored(WorkTables &tables, foundation::TransitionStamp stamp);

// The goals' due work, one item and transition each (ADR-0020).
struct DueWork {
    enum class Kind {
        expire, // the goal's expiry has come
        derive, // its due occurrences become a task
        skip,   // its due occurrences waited too long for a task
    };
    Kind kind;
    uint32_t goal_index;
    ayni_reason_t skip_reason; // skip only: derivation_blocker()
};

// Why no task can be derived for goal_index now: AYNI_REASON_TASK_HELD when
// its strategy's decision holds one of its tasks (ADR-0022), else
// AYNI_REASON_TASK_CAPACITY when the task table has no room, else
// AYNI_REASON_ID_EXHAUSTED when no id is left (id_left false), else
// AYNI_REASON_NONE.
ayni_reason_t derivation_blocker(const TaskTable &tasks, uint32_t goal_index, bool id_left);

// The due work at now_us, in this order, the first created goal (the
// smaller goal id) winning a tie: the goal that expires earliest, once its
// expiry has come; else the goal that may advance whose due work comes
// first: its next occurrence when a task can be derived for it (no
// derivation_blocker()), to be derived, or the end of that occurrence's
// wait, its occurrence_wait_max_us from when it fell due, to be skipped
// (GoalTable::skip_due). Returns false when none of that has come by
// now_us. Either way sets out_next_us to the earliest time
// one of them comes, or to no_time when none will: only a change makes
// another come, and the change says so.
bool find_due_work(const WorkTables &tables, uint64_t now_us, bool id_left, DueWork &out_work,
                   uint64_t &out_next_us);
// Derives the occurrences of goal_index due by now_us as the READY task
// task_id. Only right after find_due_work(now_us) returned derive for it.
void derive(WorkTables &tables, uint32_t goal_index, uint64_t now_us, uint64_t task_id,
            foundation::TransitionStamp stamp);
// Skips the occurrences of goal_index due by now_us for reason
// (GoalTable::skip_due). Only right after find_due_work(now_us) returned
// skip for it, with its skip_reason. If the goal then has no task that has
// not ended and no occurrence to come, it becomes FAILED in the same
// transition, its descendants that are not terminal cancelled with their
// tasks (ADR-0023), unless goal_end_exhausts: then it stays ACTIVE.
void skip(WorkTables &tables, uint32_t goal_index, uint64_t now_us, ayni_reason_t reason,
          foundation::TransitionStamp stamp);

} // namespace ayni::work

#endif // AYNI_CORE_WORK_PROPAGATION_H
