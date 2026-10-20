#include "work/propagation.h"

namespace ayni::work {
namespace {

// The state a change leaves, applied over the tables while the change is
// checked and while it is applied, so both see the same outcome.
struct After {
    uint32_t goal_index = GoalTable::no_parent;
    ayni_goal_lifecycle_t goal_lifecycle = 0;
    uint64_t instance_id = 0;
    ayni_agent_lifecycle_t instance_lifecycle = 0;
    bool ends = false;                      // the tasks it reaches end...
    TaskChange ending = TaskChange::cancel; // ...by this change
};

// Walks up at most goal_depth_max parents.
bool may_advance(const WorkTables &tables, uint32_t goal_index, uint64_t owner_instance_id,
                 const After &after) {
    ayni_agent_lifecycle_t owner = after.instance_lifecycle;
    if (owner_instance_id != after.instance_id) {
        const AgentInstance *instance = tables.agents.find_instance(owner_instance_id);
        owner = instance != nullptr ? instance->lifecycle : 0;
    }
    if (owner != AYNI_AGENT_RUNNING) {
        return false;
    }
    for (uint32_t index = goal_index; index != GoalTable::no_parent;
         index = tables.goals.at(index).parent_index) {
        const ayni_goal_lifecycle_t lifecycle =
            index == after.goal_index ? after.goal_lifecycle : tables.goals.at(index).lifecycle;
        if (lifecycle != AYNI_GOAL_ACTIVE) {
            return false;
        }
    }
    return true;
}

// The change a task the change reaches undergoes, if any.
bool settles(const WorkTables &tables, const Task &task, const After &after,
             TaskChange &out_change) {
    if (after.ends) {
        out_change = after.ending;
        return TaskTable::allows(task, out_change);
    }
    out_change = may_advance(tables, task.goal_index, task.owner_instance_id, after)
                     ? TaskChange::resume
                     : TaskChange::pause;
    return TaskTable::allows(task, out_change);
}

template <typename Reaches>
bool settling_exhausts(const WorkTables &tables, const After &after, Reaches reaches) {
    for (uint32_t index = 0; index < tables.tasks.size(); ++index) {
        const Task &task = tables.tasks.at(index);
        TaskChange change = TaskChange::cancel;
        if (reaches(task) && settles(tables, task, after, change) &&
            TaskTable::at_largest_revision(task)) {
            return true;
        }
    }
    return false;
}

template <typename Reaches>
void settle(WorkTables &tables, const After &after, Reaches reaches,
            foundation::TransitionStamp stamp) {
    for (uint32_t index = 0; index < tables.tasks.size(); ++index) {
        const Task &task = tables.tasks.at(index);
        TaskChange change = TaskChange::cancel;
        if (reaches(task) && settles(tables, task, after, change)) {
            tables.tasks.apply(index, change, stamp);
        }
    }
}

After after_goal_change(uint32_t goal_index, GoalChange change) {
    After after;
    after.goal_index = goal_index;
    switch (change) {
    case GoalChange::pause:
        after.goal_lifecycle = AYNI_GOAL_PAUSED;
        break;
    case GoalChange::resume:
        after.goal_lifecycle = AYNI_GOAL_ACTIVE;
        break;
    case GoalChange::cancel:
        after.goal_lifecycle = AYNI_GOAL_CANCELLED;
        after.ends = true;
        after.ending = TaskChange::cancel;
        break;
    case GoalChange::expire:
        after.goal_lifecycle = AYNI_GOAL_EXPIRED;
        after.ends = true;
        after.ending = TaskChange::expire;
        break;
    case GoalChange::satisfy:
        after.goal_lifecycle = AYNI_GOAL_SATISFIED;
        after.ends = true;
        after.ending = TaskChange::cancel;
        break;
    case GoalChange::fail:
        after.goal_lifecycle = AYNI_GOAL_FAILED;
        after.ends = true;
        after.ending = TaskChange::cancel;
        break;
    }
    return after;
}

// Whether ending task_index by change ends its goal, and how. The goal of a
// READY task is ACTIVE; it ends once it has had its last occurrence and its
// last task. A recurring goal goes on after a failed task, as after a
// completed one.
bool ends_goal(const WorkTables &tables, uint32_t task_index, TaskChange change,
               GoalChange &out_change) {
    const uint32_t goal_index = tables.tasks.at(task_index).goal_index;
    const Goal &goal = tables.goals.at(goal_index);
    if (occurs_again(goal.trigger, goal.cursor) ||
        tables.tasks.live_task_besides(goal_index, task_index)) {
        return false;
    }
    out_change = change == TaskChange::complete ? GoalChange::satisfy : GoalChange::fail;
    return true;
}

After after_instance_change(uint64_t instance_id, InstanceChange change) {
    After after;
    after.instance_id = instance_id;
    switch (change) {
    case InstanceChange::pause:
        after.instance_lifecycle = AYNI_AGENT_PAUSED;
        break;
    case InstanceChange::resume:
        after.instance_lifecycle = AYNI_AGENT_RUNNING;
        break;
    case InstanceChange::stop:
        after.instance_lifecycle = AYNI_AGENT_STOPPED;
        after.ends = true;
        after.ending = TaskChange::cancel;
        break;
    }
    return after;
}

} // namespace

bool goal_change_exhausts_tasks(const WorkTables &tables, uint32_t goal_index, GoalChange change) {
    return settling_exhausts(tables, after_goal_change(goal_index, change), [&](const Task &task) {
        return tables.goals.in_subtree(task.goal_index, goal_index);
    });
}

void apply_goal_change(WorkTables &tables, uint32_t goal_index, GoalChange change,
                       foundation::TransitionStamp stamp) {
    settle(
        tables, after_goal_change(goal_index, change),
        [&](const Task &task) { return tables.goals.in_subtree(task.goal_index, goal_index); },
        stamp);
    tables.goals.apply(goal_index, change, stamp);
}

bool instance_change_exhausts(const WorkTables &tables, uint32_t instance_index,
                              InstanceChange change) {
    const uint64_t instance_id = tables.agents.at(instance_index).instance_id;
    if (change == InstanceChange::stop && tables.goals.owned_at_largest_revision(instance_id)) {
        return true;
    }
    return settling_exhausts(
        tables, after_instance_change(instance_id, change),
        [&](const Task &task) { return task.owner_instance_id == instance_id; });
}

void apply_instance_change(WorkTables &tables, uint32_t instance_index, InstanceChange change,
                           foundation::TransitionStamp stamp) {
    const uint64_t instance_id = tables.agents.at(instance_index).instance_id;
    settle(
        tables, after_instance_change(instance_id, change),
        [&](const Task &task) { return task.owner_instance_id == instance_id; }, stamp);
    if (change == InstanceChange::stop) {
        tables.goals.cancel_owned(instance_id, stamp);
    }
    tables.agents.apply(instance_index, change, stamp);
}

bool task_end_exhausts(const WorkTables &tables, uint32_t task_index, TaskChange change) {
    GoalChange goal_change = GoalChange::satisfy;
    return ends_goal(tables, task_index, change, goal_change) &&
           goal_end_exhausts(tables, tables.tasks.at(task_index).goal_index, goal_change);
}

void end_task(WorkTables &tables, uint32_t task_index, TaskChange change,
              foundation::TransitionStamp stamp) {
    GoalChange goal_change = GoalChange::satisfy;
    const bool goal_ends = ends_goal(tables, task_index, change, goal_change);
    const uint32_t goal_index = tables.tasks.at(task_index).goal_index;
    tables.tasks.apply(task_index, change, stamp);
    // The ended task is no longer one the goal's end cancels.
    if (goal_ends) {
        apply_goal_change(tables, goal_index, goal_change, stamp);
    }
}

bool goal_end_exhausts(const WorkTables &tables, uint32_t goal_index, GoalChange change) {
    // Its lifecycle allows the end, so only a revision can refuse it.
    return tables.goals.check(goal_index, change, tables.goals.at(goal_index).revision) !=
               GoalTable::ChangeCheck::allowed ||
           goal_change_exhausts_tasks(tables, goal_index, change);
}

namespace {

// What keeps every goal from deriving a task now.
ayni_reason_t table_blocker(const TaskTable &tasks, bool id_left) {
    if (!tasks.has_room()) {
        return AYNI_REASON_TASK_CAPACITY;
    }
    return id_left ? AYNI_REASON_NONE : AYNI_REASON_ID_EXHAUSTED;
}

// A held task is not decided again, so a task derived beside it would only
// wait for the same strategy, and a goal held this way at each occurrence
// would fill the task table that every goal shares.
ayni_reason_t goal_blocker(const TaskTable &tasks, uint32_t goal_index, ayni_reason_t table) {
    return tasks.holds_task_of(goal_index) ? AYNI_REASON_TASK_HELD : table;
}

} // namespace

ayni_reason_t derivation_blocker(const TaskTable &tasks, uint32_t goal_index, bool id_left) {
    return goal_blocker(tasks, goal_index, table_blocker(tasks, id_left));
}

// So an unbounded wait, added to when an occurrence fell due, never ends.
static_assert(AYNI_OCCURRENCE_WAIT_UNBOUNDED == no_time, "unbounded waits saturate");

bool find_due_work(const WorkTables &tables, uint64_t now_us, bool id_left, DueWork &out_work,
                   uint64_t &out_next_us) {
    const ayni_reason_t table = table_blocker(tables.tasks, id_left);
    // Strictly earlier throughout, so the first created wins a tie. A
    // terminal goal has neither an expiry nor a next occurrence.
    uint32_t expiring = GoalTable::no_parent;
    uint64_t expires_us = no_time;
    uint32_t advancing = GoalTable::no_parent;
    uint64_t advances_us = no_time;
    ayni_reason_t blocker = AYNI_REASON_NONE; // advancing's
    for (uint32_t index = 0; index < tables.goals.size(); ++index) {
        const Goal &goal = tables.goals.at(index);
        if (goal.expires_us < expires_us) {
            expires_us = goal.expires_us;
            expiring = index;
        }
        // A blocker only puts the due work later, so a goal whose next
        // occurrence is not earlier cannot win.
        if (goal.cursor.next_due_us >= advances_us ||
            !may_advance(tables, index, goal.owner_instance_id, After{})) {
            continue;
        }
        const ayni_reason_t goal_blocked = goal_blocker(tables.tasks, index, table);
        const uint64_t due_us =
            goal_blocked == AYNI_REASON_NONE
                ? goal.cursor.next_due_us
                : time_after(goal.cursor.next_due_us, goal.occurrence_wait_max_us);
        if (due_us < advances_us) {
            advances_us = due_us;
            advancing = index;
            blocker = goal_blocked;
        }
    }
    out_next_us = expires_us < advances_us ? expires_us : advances_us;
    // A clock at no_time has reached every time, the never of no candidate
    // included.
    if (expiring != GoalTable::no_parent && expires_us <= now_us) {
        out_work = {DueWork::Kind::expire, expiring, AYNI_REASON_NONE};
        return true;
    }
    if (advancing != GoalTable::no_parent && advances_us <= now_us) {
        out_work = {blocker == AYNI_REASON_NONE ? DueWork::Kind::derive : DueWork::Kind::skip,
                    advancing, blocker};
        return true;
    }
    return false;
}

void derive(WorkTables &tables, uint32_t goal_index, uint64_t now_us, uint64_t task_id,
            foundation::TransitionStamp stamp) {
    const TakenOccurrences taken = tables.goals.take_due(goal_index, now_us, task_id, stamp);
    const Goal &goal = tables.goals.at(goal_index);
    const NewTask task{goal.goal_id, goal.owner_instance_id, taken.due_us,
                       goal_index,   taken.latest,           taken.count};
    // Cannot fail: find_due_work checked for room.
    static_cast<void>(tables.tasks.insert(task, task_id, stamp));
}

} // namespace ayni::work
