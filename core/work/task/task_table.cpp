#include "work/task/task_table.h"

#include <cstring>
#include <limits>
#include <new>

namespace ayni::work {
namespace {

constexpr uint32_t max_revision = std::numeric_limits<uint32_t>::max();

// The lifecycles this library brings a task to: RUNNING and CANCELLING come
// with requests tasks wait for (ADR-0022).
bool is_reached(ayni_task_lifecycle_t lifecycle) {
    switch (lifecycle) {
    case AYNI_TASK_READY:
    case AYNI_TASK_PAUSED:
    case AYNI_TASK_COMPLETED:
    case AYNI_TASK_CANCELLED:
    case AYNI_TASK_FAILED:
    case AYNI_TASK_EXPIRED:
        return true;
    default:
        return false;
    }
}

static_assert(sizeof(Task) % alignof(catalog::CatalogId) == 0,
              "the steps after the tasks are aligned");

std::size_t steps_size(const TaskTable::Limits &limits) {
    return sizeof(catalog::CatalogId) * limits.capacity * limits.step_capacity;
}

} // namespace

std::size_t TaskTable::storage_size(const Limits &limits) {
    return foundation::BoundedArray<Task>::storage_size(limits.capacity) + steps_size(limits) +
           static_cast<std::size_t>(limits.capacity) * limits.checkpoint_max_bytes;
}

TaskTable::TaskTable(void *storage, const Limits &limits)
    : tasks_(storage, limits.capacity),
      steps_(reinterpret_cast<catalog::CatalogId *>(
          static_cast<uint8_t *>(storage) +
          foundation::BoundedArray<Task>::storage_size(limits.capacity))),
      checkpoints_(reinterpret_cast<uint8_t *>(steps_) + steps_size(limits)),
      step_capacity_(limits.step_capacity), checkpoint_max_bytes_(limits.checkpoint_max_bytes) {
    const uint32_t steps = limits.capacity * limits.step_capacity;
    for (uint32_t index = 0; index < steps; ++index) {
        new (steps_ + index) catalog::CatalogId();
    }
}

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
    uint32_t slot = 0;
    uint64_t replaced_task_id = 0;
    if (tasks_.full()) {
        const uint32_t oldest = giving_way();
        if (oldest == no_index) {
            return false;
        }
        slot = tasks_[oldest].slot;
        replaced_task_id = tasks_[oldest].task_id;
        tasks_.remove_at(oldest);
    } else {
        slot = free_slot();
    }
    place(task, task_id, replaced_task_id, slot, stamp);
    return true;
}

void TaskTable::place(const NewTask &task, uint64_t task_id, uint64_t replaced_task_id,
                      uint32_t slot, foundation::TransitionStamp stamp) {
    Task created{};
    created.task_id = task_id;
    created.goal_id = task.goal_id;
    created.owner_instance_id = task.owner_instance_id;
    created.replaced_task_id = replaced_task_id;
    created.created_transition_id = stamp.transition_id;
    created.commit_seq = stamp.commit_seq;
    created.goal_index = task.goal_index;
    created.occurrence = task.occurrence;
    created.occurrence_count = task.occurrence_count;
    created.due_us = task.due_us;
    created.revision = 1;
    created.lifecycle = AYNI_TASK_READY;
    created.slot = slot;
    // Cannot fail: the caller made room.
    static_cast<void>(tasks_.push_back(created));
}

void TaskTable::remove_goal(uint32_t goal_index) {
    // Downwards, so the task that remove_at() moves into a place has been
    // looked at already.
    for (uint32_t index = tasks_.size(); index > 0U; --index) {
        if (tasks_[index - 1U].goal_index == goal_index) {
            tasks_.remove_at(index - 1U);
        }
    }
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
    if (still_held(task)) {
        --held_count_;
    }
    // An explicit terminal change preserves the visible diagnostic, as it
    // did before compatibility holds became a separate boot overlay.
    if (task.recovery_hold_status != AYNI_OK) {
        task.hold_status = task.recovery_hold_status;
        task.hold_reason = task.recovery_hold_reason;
        task.recovery_hold_status = AYNI_OK;
        task.recovery_hold_reason = AYNI_REASON_NONE;
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

void TaskTable::authority_changed(uint32_t index, foundation::TransitionStamp stamp) {
    tasks_[index].commit_seq = stamp.commit_seq;
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

bool TaskTable::add_step(uint32_t index, ayni_string_v1 key, foundation::TransitionStamp stamp) {
    Task &task = tasks_[index];
    if (task.step_count == step_capacity_ || has_step(index, key) ||
        !steps_of(index)[task.step_count].assign(key)) {
        return false;
    }
    if (task.steps_seq != stamp.commit_seq) {
        task.steps_seq = stamp.commit_seq;
        task.steps_before = task.step_count;
    }
    ++task.step_count;
    return true;
}

void TaskTable::set_checkpoint(uint32_t index, const catalog::CatalogId &writer_id,
                               uint32_t writer_version, ayni_bytes_v1 state,
                               foundation::TransitionStamp stamp) {
    Task &task = tasks_[index];
    if (state.size > 0U) {
        std::memcpy(checkpoints_ + static_cast<std::size_t>(task.slot) * checkpoint_max_bytes_,
                    state.data, state.size);
    }
    task.checkpoint_size = state.size;
    task.checkpoint_writer_id = writer_id;
    task.checkpoint_writer_version = writer_version;
    task.checkpoint_seq = stamp.commit_seq;
}

void TaskTable::progress(uint32_t index, foundation::TransitionStamp stamp) {
    Task &task = tasks_[index];
    ++task.revision;
    task.commit_seq = stamp.commit_seq;
}

const catalog::CatalogId &TaskTable::step(uint32_t index, uint32_t step) const {
    return steps_of(index)[step];
}

bool TaskTable::has_step(uint32_t index, ayni_string_v1 key) const {
    const catalog::CatalogId *steps = steps_of(index);
    for (uint32_t step = 0; step < tasks_[index].step_count; ++step) {
        if (steps[step].equals(key)) {
            return true;
        }
    }
    return false;
}

ayni_bytes_v1 TaskTable::checkpoint(uint32_t index) const {
    const Task &task = tasks_[index];
    return {checkpoints_ + static_cast<std::size_t>(task.slot) * checkpoint_max_bytes_,
            task.checkpoint_size};
}

// The tasks hold fewer than capacity distinct slots, so one of 0 to size()
// is free.
uint32_t TaskTable::free_slot() const {
    static_assert(AYNI_TASK_CAPACITY_MAX % 32U == 0U, "whole words");
    uint32_t held[AYNI_TASK_CAPACITY_MAX / 32U] = {};
    for (uint32_t index = 0; index < tasks_.size(); ++index) {
        const uint32_t slot = tasks_[index].slot;
        held[slot / 32U] |= 1U << (slot % 32U);
    }
    uint32_t slot = 0;
    while (((held[slot / 32U] >> (slot % 32U)) & 1U) != 0U) {
        ++slot;
    }
    return slot;
}

// An ended task never changes again, so the one with the smallest
// commit_seq ended first; ids break ties so that a replay of the log, whose
// tasks may lie in other places, gives the same one.
uint32_t TaskTable::giving_way() const {
    uint32_t oldest = no_index;
    for (uint32_t index = 0; index < tasks_.size(); ++index) {
        const Task &ended = tasks_[index];
        if (is_live(ended.lifecycle)) {
            continue;
        }
        if (oldest == no_index || ended.commit_seq < tasks_[oldest].commit_seq ||
            (ended.commit_seq == tasks_[oldest].commit_seq &&
             ended.task_id < tasks_[oldest].task_id)) {
            oldest = index;
        }
    }
    return oldest;
}

catalog::CatalogId *TaskTable::steps_of(uint32_t index) const {
    return steps_ + static_cast<std::size_t>(tasks_[index].slot) * step_capacity_;
}

// How RUNNING and CANCELLING tasks pause and cancel comes with the requests
// tasks wait for (ADR-0022). A hold waits for someone to end the task:
// nothing its strategy sees has changed but the revision and the time, so
// releasing it would mostly bring the same refusal again.
bool TaskTable::allows(const Task &task, TaskChange change) {
    const ayni_task_lifecycle_t lifecycle = effective_lifecycle(task);
    switch (change) {
    case TaskChange::pause:
    case TaskChange::complete:
    case TaskChange::fail:
        return lifecycle == AYNI_TASK_READY;
    case TaskChange::resume:
        return lifecycle == AYNI_TASK_PAUSED && !is_held(task);
    case TaskChange::cancel:
    case TaskChange::expire:
        return lifecycle == AYNI_TASK_READY || lifecycle == AYNI_TASK_PAUSED;
    }
    return false;
}

bool TaskTable::at_largest_revision(const Task &task) {
    return task.revision == max_revision;
}

bool TaskTable::next_ready(uint64_t ready_before, uint32_t &out_index, ReadyFilter filter) const {
    uint32_t next = no_index;
    for (uint32_t index = 0; index < tasks_.size(); ++index) {
        const Task &task = tasks_[index];
        if (effective_lifecycle(task) != AYNI_TASK_READY || task.commit_seq >= ready_before ||
            at_largest_revision(task) ||
            (filter.accepts != nullptr && !filter.accepts(filter.context, task))) {
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

bool TaskTable::has_ready(ReadyFilter filter) const {
    uint32_t index = no_index;
    return next_ready(std::numeric_limits<uint64_t>::max(), index, filter);
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

bool TaskTable::find(uint64_t task_id, uint32_t &out_index) const {
    for (uint32_t index = 0; index < tasks_.size(); ++index) {
        if (tasks_[index].task_id == task_id) {
            out_index = index;
            return true;
        }
    }
    return false;
}

TaskTable::Restore TaskTable::restore(const NewTask &task, uint64_t task_id,
                                      uint64_t replaced_task_id,
                                      foundation::TransitionStamp stamp) {
    uint32_t existing = 0;
    if (task_id == 0U || task.occurrence == 0U || task.occurrence_count == 0U ||
        task.occurrence_count > task.occurrence || find(task_id, existing)) {
        return Restore::corrupt;
    }
    if (replaced_task_id == 0U) {
        if (tasks_.full()) {
            return Restore::no_room;
        }
        place(task, task_id, 0, free_slot(), stamp);
        return Restore::restored;
    }
    uint32_t replaced = 0;
    if (!find(replaced_task_id, replaced) || is_live(tasks_[replaced].lifecycle)) {
        return Restore::corrupt;
    }
    const uint32_t slot = tasks_[replaced].slot;
    tasks_.remove_at(replaced);
    place(task, task_id, replaced_task_id, slot, stamp);
    return Restore::restored;
}

TaskTable::Restore TaskTable::restore_state(uint32_t index, const TaskState &state,
                                            foundation::TransitionStamp stamp) {
    Task &task = tasks_[index];
    const bool held = state.hold_status != AYNI_OK;
    // An ended task never changes again, and a held one takes no change but
    // an end (allows()).
    if (!is_live(task.lifecycle) || (still_held(task) && is_live(state.lifecycle)) ||
        !is_reached(state.lifecycle) || state.revision < task.revision ||
        state.step_count != task.step_count || state.cancel_revision > state.revision ||
        (state.cancel_revision != 0U) != (state.lifecycle == AYNI_TASK_CANCELLED) ||
        (!held && state.hold_reason != AYNI_REASON_NONE) ||
        (held && state.lifecycle == AYNI_TASK_READY)) {
        return Restore::corrupt;
    }
    if (still_held(task)) {
        --held_count_;
    }
    task.lifecycle = state.lifecycle;
    task.revision = state.revision;
    task.cancel_revision = state.cancel_revision;
    task.hold_status = state.hold_status;
    task.hold_reason = state.hold_reason;
    task.commit_seq = stamp.commit_seq;
    if (still_held(task)) {
        ++held_count_;
    }
    return Restore::restored;
}

void TaskTable::reauthorize(uint32_t index, foundation::TransitionStamp stamp) {
    auto &task = tasks_[index];
    const bool was_held = still_held(task);
    ++task.revision;
    task.commit_seq = stamp.commit_seq;
    if (task.recovery_hold_reason == AYNI_REASON_LEGACY_UNCONFIRMED) {
        task.recovery_hold_status = AYNI_OK;
        task.recovery_hold_reason = AYNI_REASON_NONE;
    }
    if (was_held && !still_held(task)) {
        --held_count_;
    }
}

TaskTable::Restore TaskTable::restore_authority_state(uint32_t index, const TaskState &state,
                                                      foundation::TransitionStamp stamp) {
    const auto &task = tasks_[index];
    if (!is_live(task.lifecycle) || state.lifecycle != task.lifecycle ||
        state.revision != task.revision || state.cancel_revision != task.cancel_revision ||
        state.hold_status != task.hold_status || state.hold_reason != task.hold_reason ||
        state.step_count != task.step_count) {
        return Restore::corrupt;
    }
    authority_changed(index, stamp);
    return Restore::restored;
}

TaskTable::Restore TaskTable::restore_reauthorization_state(uint32_t index, const TaskState &state,
                                                            foundation::TransitionStamp stamp) {
    const auto &task = tasks_[index];
    if (!is_live(task.lifecycle) || at_largest_revision(task) ||
        stamp.commit_seq <= task.commit_seq || state.revision != task.revision + 1U ||
        state.lifecycle != task.lifecycle || state.cancel_revision != task.cancel_revision ||
        state.hold_status != task.hold_status || state.hold_reason != task.hold_reason ||
        state.step_count != task.step_count) {
        return Restore::corrupt;
    }
    reauthorize(index, stamp);
    return Restore::restored;
}

TaskTable::Restore TaskTable::restore_snapshot(const NewTask &task, uint64_t task_id,
                                               uint64_t replaced_task_id,
                                               foundation::TransitionStamp stamp,
                                               uint64_t steps_seq, uint64_t checkpoint_seq,
                                               uint32_t steps_before) {
    uint32_t existing = 0;
    if (task_id == 0U || task.occurrence == 0U || task.occurrence_count == 0U ||
        task.occurrence_count > task.occurrence || find(task_id, existing) ||
        steps_seq > stamp.commit_seq || checkpoint_seq > stamp.commit_seq) {
        return Restore::corrupt;
    }
    if (tasks_.full()) {
        return Restore::no_room;
    }
    place(task, task_id, replaced_task_id, free_slot(), stamp);
    Task &restored = tasks_[tasks_.size() - 1U];
    restored.steps_seq = steps_seq;
    restored.checkpoint_seq = checkpoint_seq;
    restored.steps_before = steps_before;
    return Restore::restored;
}

TaskTable::Restore TaskTable::restore_step(uint32_t index, uint32_t step, ayni_string_v1 key,
                                           foundation::TransitionStamp stamp) {
    const Task &task = tasks_[index];
    if (step != task.step_count) {
        return Restore::corrupt;
    }
    if (task.step_count == step_capacity_) {
        return Restore::no_room;
    }
    return add_step(index, key, stamp) ? Restore::restored : Restore::corrupt;
}

TaskTable::Restore TaskTable::restore_checkpoint(uint32_t index, ayni_string_v1 writer_id,
                                                 uint32_t writer_version, ayni_bytes_v1 state,
                                                 foundation::TransitionStamp stamp) {
    catalog::CatalogId writer;
    if (writer_version == 0U || !writer.assign(writer_id)) {
        return Restore::corrupt;
    }
    if (state.size > checkpoint_max_bytes_) {
        return Restore::no_room;
    }
    set_checkpoint(index, writer, writer_version, state, stamp);
    return Restore::restored;
}

void TaskTable::hold_restored(uint32_t index, ayni_status_t status, ayni_reason_t reason) {
    Task &task = tasks_[index];
    if (!is_live(task.lifecycle) || still_held(task)) {
        return;
    }
    task.recovery_hold_status = status;
    task.recovery_hold_reason = reason;
    ++held_count_;
}

void TaskTable::reanchor(uint64_t now_us) {
    for (uint32_t index = 0; index < tasks_.size(); ++index) {
        tasks_[index].due_us = now_us;
    }
}

void TaskTable::clear() {
    tasks_.clear();
    held_count_ = 0;
}

bool TaskTable::is_live(ayni_task_lifecycle_t lifecycle) {
    return lifecycle == AYNI_TASK_READY || lifecycle == AYNI_TASK_RUNNING ||
           lifecycle == AYNI_TASK_PAUSED || lifecycle == AYNI_TASK_CANCELLING;
}

} // namespace ayni::work
