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
    bool cancel = false; // the tasks it reaches are cancelled
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
    if (after.cancel) {
        out_change = TaskChange::cancel;
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
        after.cancel = true;
        break;
    }
    return after;
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
        after.cancel = true;
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

bool find_derivable(const WorkTables &tables, uint32_t &out_goal_index) {
    if (!tables.tasks.has_room()) {
        return false;
    }
    for (uint32_t index = 0; index < tables.goals.size(); ++index) {
        const Goal &goal = tables.goals.at(index);
        if (tables.goals.has_due_occurrence(index) && !GoalTable::at_largest_revision(goal) &&
            may_advance(tables, index, goal.owner_instance_id, After{})) {
            out_goal_index = index;
            return true;
        }
    }
    return false;
}

void derive(WorkTables &tables, uint32_t goal_index, uint64_t task_id,
            foundation::TransitionStamp stamp) {
    const Goal &goal = tables.goals.at(goal_index);
    const NewTask task{goal.goal_id, goal.owner_instance_id, goal_index, goal.occurrences + 1U};
    // Cannot fail: find_derivable checked for room.
    static_cast<void>(tables.tasks.insert(task, task_id, stamp));
    tables.goals.record_occurrence(goal_index, task_id, stamp);
}

} // namespace ayni::work
