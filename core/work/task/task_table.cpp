#include "work/task/task_table.h"

#include <limits>

namespace ayni::work {
namespace {

constexpr uint32_t max_revision = std::numeric_limits<uint32_t>::max();
constexpr uint32_t no_index = std::numeric_limits<uint32_t>::max();

} // namespace

bool TaskTable::has_room() const {
    if (!tasks_.full()) {
        return true;
    }
    for (uint32_t index = 0; index < tasks_.size(); ++index) {
        if (!is_live(tasks_[index].lifecycle)) {
            return true;
        }
    }
    return false;
}

bool TaskTable::insert(const NewTask &task, uint64_t task_id, foundation::TransitionStamp stamp) {
    if (tasks_.full()) {
        // An ended task never changes again, so the one with the smallest
        // commit_seq ended first.
        uint32_t oldest = no_index;
        for (uint32_t index = 0; index < tasks_.size(); ++index) {
            const Task &ended = tasks_[index];
            if (!is_live(ended.lifecycle) &&
                (oldest == no_index || ended.commit_seq < tasks_[oldest].commit_seq)) {
                oldest = index;
            }
        }
        if (oldest == no_index) {
            return false;
        }
        tasks_.remove_at(oldest);
    }
    Task created{};
    created.task_id = task_id;
    created.goal_id = task.goal_id;
    created.owner_instance_id = task.owner_instance_id;
    created.created_transition_id = stamp.transition_id;
    created.commit_seq = stamp.commit_seq;
    created.goal_index = task.goal_index;
    created.occurrence = task.occurrence;
    created.revision = 1;
    created.lifecycle = AYNI_TASK_READY;
    // Cannot fail: there is room now.
    static_cast<void>(tasks_.push_back(created));
    return true;
}

TaskTable::ChangeCheck TaskTable::check(uint32_t index, TaskChange change,
                                        uint32_t expected_revision) const {
    const Task &task = tasks_[index];
    if (expected_revision != task.revision) {
        return ChangeCheck::revision_conflict;
    }
    if (!allows(task, change)) {
        return ChangeCheck::invalid_state;
    }
    if (at_largest_revision(task)) {
        return ChangeCheck::revision_exhausted;
    }
    return ChangeCheck::allowed;
}

void TaskTable::apply(uint32_t index, TaskChange change, foundation::TransitionStamp stamp) {
    Task &task = tasks_[index];
    switch (change) {
    case TaskChange::pause:
        task.lifecycle = AYNI_TASK_PAUSED;
        break;
    case TaskChange::resume:
        task.lifecycle = AYNI_TASK_READY;
        break;
    case TaskChange::cancel:
        task.lifecycle = AYNI_TASK_CANCELLED;
        break;
    }
    ++task.revision;
    task.commit_seq = stamp.commit_seq;
    if (task.lifecycle == AYNI_TASK_CANCELLED) {
        task.cancel_revision = task.revision;
    }
}

// How RUNNING and CANCELLING tasks pause and cancel comes with strategies
// and cancellable requests (ADR-0018).
bool TaskTable::allows(const Task &task, TaskChange change) {
    switch (change) {
    case TaskChange::pause:
        return task.lifecycle == AYNI_TASK_READY;
    case TaskChange::resume:
        return task.lifecycle == AYNI_TASK_PAUSED;
    case TaskChange::cancel:
        return task.lifecycle == AYNI_TASK_READY || task.lifecycle == AYNI_TASK_PAUSED;
    }
    return false;
}

bool TaskTable::at_largest_revision(const Task &task) {
    return task.revision == max_revision;
}

bool TaskTable::find(uint64_t task_id, uint32_t &out_index) const {
    for (uint32_t index = 0; index < tasks_.size(); ++index) {
        if (tasks_[index].task_id == task_id) {
            out_index = index;
            return true;
        }
    }
    return false;
}

bool TaskTable::is_live(ayni_task_lifecycle_t lifecycle) {
    return lifecycle == AYNI_TASK_READY || lifecycle == AYNI_TASK_RUNNING ||
           lifecycle == AYNI_TASK_PAUSED || lifecycle == AYNI_TASK_CANCELLING;
}

} // namespace ayni::work
