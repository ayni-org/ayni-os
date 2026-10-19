// How changes to goals and instances reach their tasks, and which goal
// derives the next task (ADR-0018). The goal, task and agent tables each keep
// their own invariants; the rule here spans them: a task may advance only
// while its goal and all the goal's ancestors are ACTIVE and its owner
// instance is RUNNING. A change re-settles every READY or PAUSED task it
// reaches against that rule in the same transition, and a cancel takes the
// tasks it reaches with it.
//
// The kernel calls these inside one transition on the owner thread: first
// the exhausts_* check, then apply_*, which uses the same reach and the same
// rule, so a change that passes the check never stops halfway.

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

// The goal whose next occurrence becomes a task: the first in creation order
// with a due occurrence that may advance, provided the task table has room.
// Returns false when there is none.
bool find_derivable(const WorkTables &tables, uint32_t &out_goal_index);
// Derives that occurrence as the READY task task_id. Only right after
// find_derivable returned goal_index.
void derive(WorkTables &tables, uint32_t goal_index, uint64_t task_id,
            foundation::TransitionStamp stamp);

} // namespace ayni::work

#endif // AYNI_CORE_WORK_PROPAGATION_H
