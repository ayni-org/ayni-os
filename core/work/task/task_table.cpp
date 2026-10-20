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
    created.occurrence_count = task.occurrence_count;
    created.due_us = task.due_us;
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
    // A held task takes no change but an end (allows()).
    if (is_held(task)) {
        --held_count_;
    }
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
    case TaskChange::expire:
        task.lifecycle = AYNI_TASK_EXPIRED;
        break;
    case TaskChange::complete:
        task.lifecycle = AYNI_TASK_COMPLETED;
        break;
    case TaskChange::fail:
        task.lifecycle = AYNI_TASK_FAILED;
        break;
    }
    ++task.revision;
    task.commit_seq = stamp.commit_seq;
    if (task.lifecycle == AYNI_TASK_CANCELLED) {
        task.cancel_revision = task.revision;
    }
}

void TaskTable::hold(uint32_t index, ayni_status_t status, ayni_reason_t reason,
                     foundation::TransitionStamp stamp) {
    Task &task = tasks_[index];
    task.lifecycle = AYNI_TASK_PAUSED;
    task.hold_status = status;
    task.hold_reason = reason;
    ++task.revision;
    task.commit_seq = stamp.commit_seq;
    ++held_count_;
}

// How RUNNING and CANCELLING tasks pause and cancel comes with the requests
// tasks wait for (ADR-0022). A hold waits for someone to end the task:
// nothing its strategy sees has changed but the revision and the time, so
// releasing it would mostly bring the same refusal again.
bool TaskTable::allows(const Task &task, TaskChange change) {
    switch (change) {
    case TaskChange::pause:
    case TaskChange::complete:
    case TaskChange::fail:
        return task.lifecycle == AYNI_TASK_READY;
    case TaskChange::resume:
        return task.lifecycle == AYNI_TASK_PAUSED && !is_held(task);
    case TaskChange::cancel:
    case TaskChange::expire:
        return task.lifecycle == AYNI_TASK_READY || task.lifecycle == AYNI_TASK_PAUSED;
    }
    return false;
}

bool TaskTable::at_largest_revision(const Task &task) {
    return task.revision == max_revision;
}

bool TaskTable::next_ready(uint64_t ready_before, uint32_t &out_index) const {
    uint32_t next = no_index;
    for (uint32_t index = 0; index < tasks_.size(); ++index) {
        const Task &task = tasks_[index];
        if (task.lifecycle != AYNI_TASK_READY || task.commit_seq >= ready_before ||
            at_largest_revision(task)) {
            continue;
        }
        // One transition can make several tasks READY, so ids break ties.
        if (next == no_index || task.commit_seq < tasks_[next].commit_seq ||
            (task.commit_seq == tasks_[next].commit_seq && task.task_id < tasks_[next].task_id)) {
            next = index;
        }
    }
    out_index = next;
    return next != no_index;
}

bool TaskTable::has_ready() const {
    for (uint32_t index = 0; index < tasks_.size(); ++index) {
        if (tasks_[index].lifecycle == AYNI_TASK_READY && !at_largest_revision(tasks_[index])) {
            return true;
        }
    }
    return false;
}

bool TaskTable::live_task_besides(uint32_t goal_index, uint32_t task_index) const {
    for (uint32_t index = 0; index < tasks_.size(); ++index) {
        if (index != task_index && tasks_[index].goal_index == goal_index &&
            is_live(tasks_[index].lifecycle)) {
            return true;
        }
    }
    return false;
}

bool TaskTable::holds_task_of(uint32_t goal_index) const {
    if (held_count_ == 0U) {
        return false;
    }
    for (uint32_t index = 0; index < tasks_.size(); ++index) {
        const Task &task = tasks_[index];
        if (task.goal_index == goal_index && task.lifecycle == AYNI_TASK_PAUSED && is_held(task)) {
            return true;
        }
    }
    return false;
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
