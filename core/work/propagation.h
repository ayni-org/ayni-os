// How changes to goals and instances reach their tasks, how a task's end
// reaches its goal, and which of the goals' due work comes next (ADR-0018,
// ADR-0020, ADR-0022). The goal, task and agent tables each keep their own
// invariants; the rule here spans them: a task may advance only while its
// goal and all the goal's ancestors are ACTIVE and its owner instance is
// RUNNING. A change re-settles every READY or PAUSED task it reaches against
// that rule in the same transition, except a task its strategy's decision
// holds, and a goal's end takes the tasks it reaches with it.
//
// The kernel calls these inside one transition on the owner thread: first
// the exhausts check, then apply_* or end_task, which use the same reach
// and the same rule, so a change that passes the check never stops halfway.

#ifndef AYNI_CORE_WORK_PROPAGATION_H
#define AYNI_CORE_WORK_PROPAGATION_H

#include "foundation/transition_stamp.h"
#include "work/agent/agent_table.h"
#include "work/goal/goal_table.h"
#include "work/task/task_table.h"

#include <cstdint>

namespace ayni::work {

struct WorkTables {
    GoalTable &goals;
    TaskTable &tasks;
    AgentTable &agents;
};

// Whether a task the change would reach is at the largest revision. The goal
// table checks the goals themselves (GoalTable::check).
bool goal_change_exhausts_tasks(const WorkTables &tables, uint32_t goal_index, GoalChange change);
// Only after GoalTable::check allowed the change and
// goal_change_exhausts_tasks returned false.
void apply_goal_change(WorkTables &tables, uint32_t goal_index, GoalChange change,
                       foundation::TransitionStamp stamp);

// Whether a task, or for stop a goal, the change would reach is at the
// largest revision. The agent table checks the instance itself.
bool instance_change_exhausts(const WorkTables &tables, uint32_t instance_index,
                              InstanceChange change);
// Only after AgentTable::check allowed the change and
// instance_change_exhausts returned false. Stop cancels every goal and task
// the instance owns that is not yet terminal.
void apply_instance_change(WorkTables &tables, uint32_t instance_index, InstanceChange change,
                           foundation::TransitionStamp stamp);

// Whether ending the READY task task_index, below the largest revision, by
// change, complete or fail, would raise a revision past the largest: when
// its goal ends with it, that of the goal, of a descendant or of a task the
// goal's end reaches. A task at the largest revision is never decided
// (TaskTable::next_ready).
bool task_end_exhausts(const WorkTables &tables, uint32_t task_index, TaskChange change);
// Only after task_end_exhausts returned false. Ends the task. If its goal
// then has no task that has not ended and no occurrence to come, the goal
// ends too, SATISFIED for complete and FAILED for fail, and its descendants
// that are not terminal are cancelled with their tasks.
void end_task(WorkTables &tables, uint32_t task_index, TaskChange change,
              foundation::TransitionStamp stamp);

// Whether ending goal_index by change, satisfy or fail while it is ACTIVE or
// expire while it is not terminal, would raise a revision past the largest:
// the goal's own, a descendant's or that of a task the end reaches. Nothing
// lowers a revision, so such a goal never could end that way.
bool goal_end_exhausts(const WorkTables &tables, uint32_t goal_index, GoalChange change);

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

// The due work at now_us, in this order, the first created goal winning a
// tie: the goal that expires earliest, once its expiry has come; else the
// goal that may advance whose due work comes first: its next occurrence when
// a task can be derived for it (no derivation_blocker()), to be derived, or
// the end of that occurrence's wait, its occurrence_wait_max_us from when it
// fell due, to be skipped (GoalTable::skip_due). Returns false when none of
// that has come by now_us. Either way sets out_next_us to the earliest time
// one of them comes, or to no_time when none will: only a change makes
// another come, and the change says so.
bool find_due_work(const WorkTables &tables, uint64_t now_us, bool id_left, DueWork &out_work,
                   uint64_t &out_next_us);
// Derives the occurrences of goal_index due by now_us as the READY task
// task_id. Only right after find_due_work(now_us) returned derive for it.
void derive(WorkTables &tables, uint32_t goal_index, uint64_t now_us, uint64_t task_id,
            foundation::TransitionStamp stamp);

} // namespace ayni::work

#endif // AYNI_CORE_WORK_PROPAGATION_H
