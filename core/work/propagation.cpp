#include "work/propagation.h"

namespace ayni::work {
namespace {

// The state a change leaves, applied over the tables while the change is
// checked and while it is applied, so both see the same outcome.
struct After {
    uint32_t goal_index = GoalTable::no_index;
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
        owner = instance != nullptr ? AgentTable::effective_lifecycle(*instance) : 0;
    }
    if (owner != AYNI_AGENT_RUNNING) {
        return false;
    }
    for (uint32_t index = goal_index; index != GoalTable::no_parent;
         index = tables.goals.at(index).parent_index) {
        const ayni_goal_lifecycle_t lifecycle =
            index == after.goal_index ? after.goal_lifecycle : tables.goals.at(index).lifecycle;
        if (lifecycle != AYNI_GOAL_ACTIVE || GoalTable::is_held(tables.goals.at(index))) {
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

// Whether goal_index has had its last occurrence and has no task that has
// not ended besides task_index (TaskTable::no_index: none is ending), so
// that nothing more will happen to it but a change from outside.
bool has_had_its_last(const WorkTables &tables, uint32_t goal_index, uint32_t task_index) {
    const Goal &goal = tables.goals.at(goal_index);
    return !occurs_again(goal.trigger, goal.cursor) &&
           !tables.tasks.live_task_besides(goal_index, task_index);
}

// Whether ending task_index by change ends its goal, and how: the goal ends
// once it has had its last occurrence and its last task, as that task did
// (ADR-0022, ADR-0023). A recurring goal goes on after a failed or cancelled
// task, as after a completed one. The goal of a READY task is ACTIVE; a
// cancelled task's goal may also be PAUSED, and cancel ends either.
bool ends_goal(const WorkTables &tables, uint32_t task_index, TaskChange change,
               GoalChange &out_change) {
    switch (change) {
    case TaskChange::complete:
        out_change = GoalChange::satisfy;
        break;
    case TaskChange::fail:
        out_change = GoalChange::fail;
        break;
    case TaskChange::cancel:
        out_change = GoalChange::cancel;
        break;
    case TaskChange::pause:
    case TaskChange::resume:
    case TaskChange::expire: // only with its goal's own end
        return false;
    }
    return has_had_its_last(tables, tables.tasks.at(task_index).goal_index, task_index);
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

bool insert_goal(WorkTables &tables, const NewGoal &goal, uint64_t goal_id,
                 foundation::TransitionStamp stamp, uint32_t &out_index) {
    const GoalTable::Placement placement = tables.goals.insert(goal, goal_id, stamp, out_index);
    if (placement == GoalTable::Placement::given_way) {
        tables.tasks.remove_goal(out_index);
    }
    return placement != GoalTable::Placement::refused;
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

// The goals with a still_held() task, gathered in one pass over the tasks so
// that a search looks at each task once, not once for each goal it weighs.
// Goal indexes are below AYNI_GOAL_CAPACITY_MAX.
class HeldGoals {
  public:
    explicit HeldGoals(const TaskTable &tasks) {
        if (!tasks.has_held()) {
            return;
        }
        for (uint32_t index = 0; index < tasks.size(); ++index) {
            const Task &task = tasks.at(index);
            if (TaskTable::still_held(task)) {
                bits_[task.goal_index / 32U] |= 1U << (task.goal_index % 32U);
            }
        }
    }

    bool contains(uint32_t goal_index) const {
        return ((bits_[goal_index / 32U] >> (goal_index % 32U)) & 1U) != 0U;
    }

  private:
    static_assert(AYNI_GOAL_CAPACITY_MAX % 32U == 0U, "whole words");
    uint32_t bits_[AYNI_GOAL_CAPACITY_MAX / 32U] = {};
};

// A held task is not decided again, so a task derived beside it would only
// wait for the same strategy, and a goal held this way at each occurrence
// would fill the task table that every goal shares.
ayni_reason_t goal_blocker(const HeldGoals &held, uint32_t goal_index, ayni_reason_t table) {
    return held.contains(goal_index) ? AYNI_REASON_TASK_HELD : table;
}

// Whether goal index, at time_us, comes before goal best, at best_us:
// earlier, or as early and created first. Goal ids give the order of
// creation, places do not (ADR-0025). best is GoalTable::no_index, and
// best_us no_time, until a goal comes, and nothing comes at no_time.
bool comes_first(const GoalTable &goals, uint64_t time_us, uint32_t index, uint64_t best_us,
                 uint32_t best) {
    if (time_us != best_us) {
        return time_us < best_us;
    }
    return best != GoalTable::no_index && goals.at(index).goal_id < goals.at(best).goal_id;
}

} // namespace

bool settles_restored(const WorkTables &tables) {
    for (uint32_t index = 0; index < tables.tasks.size(); ++index) {
        const Task &task = tables.tasks.at(index);
        TaskChange change = TaskChange::cancel;
        if (!TaskTable::at_largest_revision(task) && settles(tables, task, After{}, change)) {
            return true;
        }
    }
    return false;
}

// A task at the largest revision stays: no decision takes it
// (TaskTable::next_ready), and nothing lowers a revision.
void settle_restored(WorkTables &tables, foundation::TransitionStamp stamp) {
    settle(
        tables, After{}, [](const Task &task) { return !TaskTable::at_largest_revision(task); },
        stamp);
}

ayni_reason_t derivation_blocker(const TaskTable &tasks, uint32_t goal_index, bool id_left) {
    return goal_blocker(HeldGoals(tasks), goal_index, table_blocker(tasks, id_left));
}

// So an unbounded wait, added to when an occurrence fell due, never ends.
static_assert(AYNI_OCCURRENCE_WAIT_UNBOUNDED == no_time, "unbounded waits saturate");

bool find_due_work(const WorkTables &tables, uint64_t now_us, bool id_left, DueWork &out_work,
                   uint64_t &out_next_us) {
    const ayni_reason_t table = table_blocker(tables.tasks, id_left);
    const HeldGoals held(tables.tasks);
    // A terminal goal has neither an expiry nor a next occurrence.
    uint32_t expiring = GoalTable::no_index;
    uint64_t expires_us = no_time;
    uint32_t advancing = GoalTable::no_index;
    uint64_t advances_us = no_time;
    ayni_reason_t blocker = AYNI_REASON_NONE; // advancing's
    for (uint32_t index = 0; index < tables.goals.size(); ++index) {
        const Goal &goal = tables.goals.at(index);
        // A held goal neither expires nor takes occurrences (ADR-0026).
        if (GoalTable::is_held(goal)) {
            continue;
        }
        if (comes_first(tables.goals, goal.expires_us, index, expires_us, expiring)) {
            expires_us = goal.expires_us;
            expiring = index;
        }
        // A blocker only puts the due work later, so a goal whose next
        // occurrence does not come first cannot win.
        if (!comes_first(tables.goals, goal.cursor.next_due_us, index, advances_us, advancing) ||
            !may_advance(tables, index, goal.owner_instance_id, After{})) {
            continue;
        }
        const ayni_reason_t goal_blocked = goal_blocker(held, index, table);
        const uint64_t due_us =
            goal_blocked == AYNI_REASON_NONE
                ? goal.cursor.next_due_us
                : time_after(goal.cursor.next_due_us, goal.occurrence_wait_max_us);
        if (comes_first(tables.goals, due_us, index, advances_us, advancing)) {
            advances_us = due_us;
            advancing = index;
            blocker = goal_blocked;
        }
    }
    out_next_us = expires_us < advances_us ? expires_us : advances_us;
    // A clock at no_time has reached every time, the never of no candidate
    // included.
    if (expiring != GoalTable::no_index && expires_us <= now_us) {
        out_work = {DueWork::Kind::expire, expiring, AYNI_REASON_NONE};
        return true;
    }
    if (advancing != GoalTable::no_index && advances_us <= now_us) {
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

void skip(WorkTables &tables, uint32_t goal_index, uint64_t now_us, ayni_reason_t reason,
          foundation::TransitionStamp stamp) {
    tables.goals.skip_due(goal_index, now_us, reason, stamp);
    // Nothing more comes of a goal skipped past its last occurrence with no
    // task left, so it fails as after a failed last task. The skip raised
    // no revision, so the check sees every revision the end would raise.
    if (has_had_its_last(tables, goal_index, TaskTable::no_index) &&
        !goal_end_exhausts(tables, goal_index, GoalChange::fail)) {
        apply_goal_change(tables, goal_index, GoalChange::fail, stamp);
    }
}

} // namespace ayni::work
