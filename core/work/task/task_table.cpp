#include "work/task/task_table.h"

#include <cstring>
#include <limits>
#include <new>

namespace ayni::work {
namespace {

constexpr uint32_t max_revision = std::numeric_limits<uint32_t>::max();
constexpr uint32_t call_acceptance_revisions = 4;
constexpr uint32_t call_pending_revisions = 3;
constexpr uint32_t call_unknown_revisions = 2;
constexpr uint32_t cancel_and_drain_revisions = 2;
constexpr uint32_t subscription_wait_revisions = 3; // Wait, wake and final cancellation.
constexpr uint32_t subscription_wake_revisions = 2; // Wake retains the final cancellation.

// The selected-call state determines the remaining mandatory lifecycle writes.
uint32_t reserved_revisions(const Task &task) {
    if (task.operation_wait) {
        return subscription_wake_revisions; // Preserve wake and the final cancel (ADR-0063).
    }
    return task.call.id == 0U                     ? 0U
           : task.call.end != TaskCall::End::none ? 1U
           : task.call_unknown                    ? call_unknown_revisions
                                                  : call_pending_revisions;
}

// RUNNING/CANCELLING additionally require a retained logical call reference;
// TaskCall validates that relation when restoring its paired call state.
bool is_reached(ayni_task_lifecycle_t lifecycle) {
    switch (lifecycle) {
    case AYNI_TASK_READY:
    case AYNI_TASK_RUNNING:
    case AYNI_TASK_PAUSED:
    case AYNI_TASK_CANCELLING:
    case AYNI_TASK_COMPLETED:
    case AYNI_TASK_CANCELLED:
    case AYNI_TASK_FAILED:
    case AYNI_TASK_EXPIRED:
    case AYNI_TASK_WAITING:
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

TaskTable::TaskTable(void *storage, const Limits &limits, uint32_t max_turns)
    : tasks_(storage, limits.capacity),
      steps_(reinterpret_cast<catalog::CatalogId *>(
          static_cast<uint8_t *>(storage) +
          foundation::BoundedArray<Task>::storage_size(limits.capacity))),
      checkpoints_(reinterpret_cast<uint8_t *>(steps_) + steps_size(limits)),
      step_capacity_(limits.step_capacity), checkpoint_max_bytes_(limits.checkpoint_max_bytes),
      max_turns_(max_turns) {
    const uint32_t steps = limits.capacity * limits.step_capacity;
    for (uint32_t index = 0; index < steps; ++index) {
        new (steps_ + index) catalog::CatalogId();
    }
}

bool TaskTable::has_room() const {
    return !tasks_.full() || giving_way() != no_index;
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
    created.calendar_local_s = task.calendar_local_s;
    created.calendar_utc_s = task.calendar_utc_s;
    created.revision = 1;
    created.lifecycle = AYNI_TASK_READY;
    created.slot = slot;
    created.model_calls.limit = max_turns_;
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
    const bool ends_call =
        task.call.id != 0U && (change == TaskChange::cancel || change == TaskChange::expire);
    const bool ends_wait =
        task.operation_wait && (change == TaskChange::cancel || change == TaskChange::expire);
    if (ends_wait   ? max_revision - task.revision < 1
        : ends_call ? max_revision - task.revision < cancel_and_drain_revisions
        : (change == TaskChange::cancel || change == TaskChange::expire)
            ? max_revision - task.revision < 1U
            : at_revision_limit(task)) {
        return ChangeCheck::revision_exhausted;
    }
    return ChangeCheck::allowed;
}

void TaskTable::apply(uint32_t index, TaskChange change, foundation::TransitionStamp stamp) {
    Task &task = tasks_[index];
    // Recount the independent hold overlay across user lifecycle changes.
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
        task.lifecycle = task.operation_wait  ? AYNI_TASK_WAITING
                         : task.call.id == 0U ? AYNI_TASK_READY
                                              : AYNI_TASK_RUNNING;
        break;
    case TaskChange::cancel:
        task.lifecycle = task.call.id == 0U ? AYNI_TASK_CANCELLED : AYNI_TASK_CANCELLING;
        task.call.end = task.call.id == 0U ? TaskCall::End::none : TaskCall::End::cancel;
        break;
    case TaskChange::expire:
        task.lifecycle = task.call.id == 0U ? AYNI_TASK_EXPIRED : AYNI_TASK_CANCELLING;
        task.call.end = task.call.id == 0U ? TaskCall::End::none : TaskCall::End::expire;
        break;
    case TaskChange::complete:
        task.lifecycle = AYNI_TASK_COMPLETED;
        break;
    case TaskChange::fail:
        task.lifecycle = AYNI_TASK_FAILED;
        break;
    }
    if (!is_live(task.lifecycle)) {
        task.operation_wait.reset();
        task.volatile_state.pending_unknown.reset();
        // Discard unpersisted volatile checkpoint bytes (invariant: checkpoint_seq == 0).
        if (task.volatile_state.checkpoint_dirty) {
            std::memset(checkpoints_ + static_cast<std::size_t>(task.slot) * checkpoint_max_bytes_,
                        0, checkpoint_max_bytes_);
            task.volatile_state.checkpoint_dirty = false;
            task.volatile_state.checkpoint_size = 0;
            task.volatile_state.checkpoint_writer_version = 0;
            task.volatile_state.checkpoint_writer_id = {};
        }
    }
    ++task.revision;
    task.commit_seq = stamp.commit_seq;
    if (still_held(task)) {
        ++held_count_;
    }
    if (change == TaskChange::cancel) {
        task.cancel_revision = task.revision;
    }
}

void TaskTable::authority_changed(uint32_t index, foundation::TransitionStamp stamp) {
    tasks_[index].commit_seq = stamp.commit_seq;
}

// Preflight all mutable facts before the Kernel allocates a command transition.
ayni_status_t TaskTable::check_model_limit_raise(uint32_t index, uint32_t expected_revision,
                                                 uint32_t limit) const {
    const auto &task = tasks_[index];
    if (expected_revision != task.revision) {
        return AYNI_REVISION_CONFLICT;
    }
    if (!is_live(task.lifecycle)) {
        return AYNI_INVALID_STATE;
    }
    if (at_revision_limit(task)) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    if (limit <= task.model_calls.limit || limit > AYNI_MAX_TURNS_MAX) {
        return AYNI_INVALID_ARGUMENT;
    }
    return AYNI_OK;
}

void TaskTable::raise_model_limit(uint32_t index, uint32_t limit,
                                  foundation::TransitionStamp stamp) {
    auto &task = tasks_[index];
    const bool held = still_held(task);
    task.model_calls.limit = limit;
    task.model_calls.blocked = false;
    if (held && !still_held(task)) {
        --held_count_;
    }
    progress(index, stamp);
}

void TaskTable::accept_model_call(uint32_t index, foundation::TransitionStamp stamp) {
    ++tasks_[index].model_calls.used;
    progress(index, stamp);
}

void TaskTable::hold_model_calls(uint32_t index, foundation::TransitionStamp stamp) {
    auto &task = tasks_[index];
    const bool held = still_held(task);
    // Fold unpersisted EPHEMERAL model counts so durable blocked implies used == limit.
    fold_volatile_model_calls(index);
    task.model_calls.blocked = true;
    if (!held && still_held(task)) {
        ++held_count_;
    }
    progress(index, stamp);
}

void TaskTable::begin_call(uint32_t index, uint64_t call_id, CallKind kind,
                           foundation::TransitionStamp stamp) {
    fold_volatile_model_calls(index);
    if (kind == CallKind::model) {
        accept_model_call(index, stamp);
    } else {
        progress(index, stamp);
    }
    auto &task = tasks_[index];
    task.call = {call_id, TaskCall::End::none};
    task.lifecycle = AYNI_TASK_RUNNING;
}

void TaskTable::begin_volatile_call(uint32_t index, uint64_t call_id, CallKind kind) {
    auto &task = tasks_[index];
    if (kind == CallKind::model) {
        ++task.volatile_state.model_calls_used;
    }
    task.volatile_state.call_id = call_id;
    task.volatile_state.generation = volatile_clock_++;
}

void TaskTable::end_volatile_call(uint32_t index, uint64_t call_id) {
    auto &task = tasks_[index];
    if (task.volatile_state.call_id == call_id) {
        task.volatile_state.call_id = 0;
        task.volatile_state.generation = volatile_clock_++;
    }
}

void TaskTable::defer_volatile_unknown(uint32_t index, ayni_reason_t reason) {
    auto &task = tasks_[index];
    const bool held = still_held(task);
    task.volatile_state.pending_unknown = reason;
    if (!held && still_held(task)) {
        ++held_count_;
    }
}

bool TaskTable::next_volatile_unknown(uint32_t &index) const {
    for (uint32_t at = 0; at < tasks_.size(); ++at) {
        const auto &task = tasks_[at];
        if (task.volatile_state.pending_unknown && is_live(task.lifecycle) &&
            !at_largest_revision(task)) {
            index = at;
            return true;
        }
    }
    return false;
}

void TaskTable::commit_volatile_unknown(uint32_t index, foundation::TransitionStamp stamp) {
    auto &task = tasks_[index];
    fold_volatile_model_calls(index);
    hold(index, AYNI_RESULT_UNKNOWN, *task.volatile_state.pending_unknown, stamp);
    task.volatile_state.pending_unknown.reset();
}

void TaskTable::set_decision_waits_for_journal(uint32_t index, bool waits) {
    tasks_[index].volatile_state.decision_waits_for_journal = waits;
}

bool TaskTable::add_volatile_step(uint32_t index, ayni_string_v1 key) {
    auto &task = tasks_[index];
    if (live_step_count(task) == step_capacity_ || has_step(index, key)) {
        return false;
    }
    const uint32_t slot_pos = step_capacity_ - 1U - task.volatile_state.step_count;
    if (!steps_of(index)[slot_pos].assign(key)) {
        return false;
    }
    ++task.volatile_state.step_count;
    task.volatile_state.generation = volatile_clock_++;
    return true;
}

void TaskTable::set_volatile_checkpoint(uint32_t index, const catalog::CatalogId &writer_id,
                                        uint32_t writer_version, ayni_bytes_v1 state) {
    auto &task = tasks_[index];
    uint8_t *slot_bytes =
        checkpoints_ + static_cast<std::size_t>(task.slot) * checkpoint_max_bytes_;
    if (state.size > 0U) {
        std::memcpy(slot_bytes, state.data, state.size);
    }
    std::memset(slot_bytes + state.size, 0, checkpoint_max_bytes_ - state.size);
    task.volatile_state.checkpoint_dirty = true;
    task.volatile_state.checkpoint_size = state.size;
    task.volatile_state.checkpoint_writer_id = writer_id;
    task.volatile_state.checkpoint_writer_version = writer_version;
    task.volatile_state.generation = volatile_clock_++;
}

void TaskTable::volatile_progress(uint32_t index) {
    tasks_[index].volatile_state.generation = volatile_clock_++;
}

void TaskTable::promote_volatile_checkpoint(uint32_t index, foundation::TransitionStamp stamp) {
    auto &task = tasks_[index];
    if (!task.volatile_state.checkpoint_dirty) {
        return;
    }
    task.checkpoint_size = task.volatile_state.checkpoint_size;
    task.checkpoint_writer_id = task.volatile_state.checkpoint_writer_id;
    task.checkpoint_writer_version = task.volatile_state.checkpoint_writer_version;
    task.checkpoint_seq = stamp.commit_seq;
    task.volatile_state.checkpoint_dirty = false;
    task.volatile_state.checkpoint_size = 0;
    task.volatile_state.checkpoint_writer_version = 0;
    task.volatile_state.checkpoint_writer_id = {};
}

void TaskTable::fold_volatile_model_calls(uint32_t index) {
    auto &task = tasks_[index];
    task.model_calls.used += task.volatile_state.model_calls_used;
    task.volatile_state.model_calls_used = 0;
}

// Known terminal knowledge releases the selected reference. Cancellation does
// not become READY even if a late Provider success arrived (C-CALL-012).
void TaskTable::finish_call(uint32_t index, foundation::TransitionStamp stamp) {
    auto &task = tasks_[index];
    const bool was_held = still_held(task);
    const auto end = task.call.end;
    task.call = {};
    clear_settled_call_holds(task);
    if (end == TaskCall::End::cancel) {
        task.lifecycle = AYNI_TASK_CANCELLED;
    } else if (end == TaskCall::End::expire) {
        task.lifecycle = AYNI_TASK_EXPIRED;
    } else if (task.lifecycle == AYNI_TASK_RUNNING) {
        task.lifecycle = AYNI_TASK_READY;
    }
    progress(index, stamp);
    if (was_held && !still_held(task)) {
        --held_count_;
    }
}

void TaskTable::skip_queued_call(uint32_t index, foundation::TransitionStamp stamp) {
    apply(index, TaskChange::expire, stamp);
}

// Waiting is an overlay: user pause/resume remains an independent lifecycle.
// Preserve the accepted call, checkpoint and counter until an explicit owner resume.
void TaskTable::pause_resource_wait(uint32_t index, foundation::TransitionStamp stamp) {
    auto &task = tasks_[index];
    const bool held = still_held(task);
    task.hold_status = AYNI_EXPIRED;
    task.hold_reason = AYNI_REASON_RESOURCE_WAIT;
    progress(index, stamp);
    if (!held) {
        ++held_count_;
    }
}

ayni_status_t TaskTable::check_resume_wait(uint32_t index, uint32_t expected_revision) const {
    const auto &task = tasks_[index];
    if (task.revision != expected_revision) {
        return AYNI_REVISION_CONFLICT;
    }
    if (!waiting_for_resource(task) ||
        (call_query_ ? !call_query_(call_context_, task).queued : task.call.id == 0U) ||
        task.call.end != TaskCall::End::none || !is_live(task.lifecycle)) {
        return AYNI_INVALID_STATE;
    }
    // Resuming may wait again; keep the original acceptance's completion reserve.
    return has_revisions(task, call_acceptance_revisions) ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}

void TaskTable::resume_wait(uint32_t index, foundation::TransitionStamp stamp) {
    auto &task = tasks_[index];
    const bool held = still_held(task);
    task.hold_status = AYNI_OK;
    task.hold_reason = AYNI_REASON_NONE;
    progress(index, stamp);
    if (held && !still_held(task)) {
        --held_count_;
    }
}

// Only the wait overlay and revision change; Kernel pairs the exact owner command.
TaskTable::Restore TaskTable::restore_wait_resume(uint32_t index, const TaskState &state,
                                                  foundation::TransitionStamp stamp) {
    const auto &task = tasks_[index];
    if (check_resume_wait(index, task.revision) != AYNI_OK ||
        state.revision != task.revision + 1U || state.lifecycle != task.lifecycle ||
        state.cancel_revision != task.cancel_revision || state.hold_status != AYNI_OK ||
        state.hold_reason != AYNI_REASON_NONE || state.step_count != task.step_count ||
        state.call_unknown != task.call_unknown || !(state.model_calls == task.model_calls) ||
        state.call != task.call || state.operation_wait != task.operation_wait ||
        stamp.commit_seq <= task.commit_seq) {
        return Restore::corrupt;
    }
    resume_wait(index, stamp);
    return Restore::restored;
}

void TaskTable::hold_unknown_call(uint32_t index, foundation::TransitionStamp stamp) {
    auto &task = tasks_[index];
    if (task.call_unknown) {
        return;
    }
    const bool was_held = still_held(task);
    task.call_unknown = true;
    progress(index, stamp);
    if (!was_held && still_held(task)) {
        ++held_count_;
    }
}

// Only the runtime ceiling/hold and revision may change; the exact command
// pairing in Kernel proves that this was an owner-managed raise (ADR-0043).
TaskTable::Restore TaskTable::restore_model_limit_raise(uint32_t index, const TaskState &state,
                                                        foundation::TransitionStamp stamp) {
    const auto &task = tasks_[index];
    if (!state.model_calls.valid() || state.model_calls.blocked ||
        state.model_calls.used != task.model_calls.used || at_revision_limit(task) ||
        state.revision != task.revision + 1U || state.lifecycle != task.lifecycle ||
        state.cancel_revision != task.cancel_revision || state.hold_status != task.hold_status ||
        state.hold_reason != task.hold_reason || state.step_count != task.step_count ||
        !is_live(task.lifecycle) || stamp.commit_seq <= task.commit_seq ||
        state.model_calls.limit <= task.model_calls.limit || state.call != task.call ||
        state.operation_wait != task.operation_wait || state.call_unknown != task.call_unknown) {
        return Restore::corrupt;
    }
    raise_model_limit(index, state.model_calls.limit, stamp);
    return Restore::restored;
}

void TaskTable::hold(uint32_t index, ayni_status_t status, ayni_reason_t reason,
                     foundation::TransitionStamp stamp) {
    Task &task = tasks_[index];
    const bool held = still_held(task);
    task.lifecycle = AYNI_TASK_PAUSED;
    task.hold_status = status;
    task.hold_reason = reason;
    ++task.revision;
    task.commit_seq = stamp.commit_seq;
    if (!held) {
        ++held_count_;
    }
}

bool TaskTable::add_step(uint32_t index, ayni_string_v1 key, foundation::TransitionStamp stamp) {
    Task &task = tasks_[index];
    if (live_step_count(task) == step_capacity_ || has_step(index, key) ||
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

void TaskTable::erase_checkpoint(uint32_t index) {
    auto &task = tasks_[index];
    std::memset(checkpoints_ + static_cast<std::size_t>(task.slot) * checkpoint_max_bytes_, 0,
                checkpoint_max_bytes_);
    task.checkpoint_size = 0;
    task.volatile_state.checkpoint_dirty = false;
    task.volatile_state.checkpoint_size = 0;
    task.volatile_state.checkpoint_writer_version = 0;
    task.volatile_state.checkpoint_writer_id = {};
}
void TaskTable::set_checkpoint(uint32_t index, const catalog::CatalogId &writer_id,
                               uint32_t writer_version, ayni_bytes_v1 state,
                               foundation::TransitionStamp stamp) {
    Task &task = tasks_[index];
    if (state.size > 0U) {
        std::memcpy(checkpoints_ + static_cast<std::size_t>(task.slot) * checkpoint_max_bytes_,
                    state.data, state.size);
    }
    // Replacing a shorter checkpoint must also discard the previous private tail (ADR-0060).
    std::memset(checkpoints_ + static_cast<std::size_t>(task.slot) * checkpoint_max_bytes_ +
                    state.size,
                0, checkpoint_max_bytes_ - state.size);
    task.checkpoint_size = state.size;
    task.checkpoint_writer_id = writer_id;
    task.checkpoint_writer_version = writer_version;
    task.checkpoint_seq = stamp.commit_seq;
    task.volatile_state.checkpoint_dirty = false;
    task.volatile_state.checkpoint_size = 0;
    task.volatile_state.checkpoint_writer_version = 0;
    task.volatile_state.checkpoint_writer_id = {};
}

void TaskTable::progress(uint32_t index, foundation::TransitionStamp stamp) {
    Task &task = tasks_[index];
    ++task.revision;
    task.commit_seq = stamp.commit_seq;
}

bool TaskTable::can_wait_operation(const Task &task) {
    return task.lifecycle == AYNI_TASK_READY && task.call.id == 0 &&
           task.volatile_state.call_id == 0 && !task.operation_wait &&
           max_revision - task.revision >= subscription_wait_revisions;
}
void TaskTable::wait_operation(uint32_t index, const OperationWait &basis,
                               foundation::TransitionStamp stamp) {
    auto &task = tasks_[index];
    task.operation_wait = basis;
    task.lifecycle = AYNI_TASK_WAITING;
    progress(index, stamp);
}
bool TaskTable::wake_operation(uint32_t index, foundation::TransitionStamp stamp) {
    auto &task = tasks_[index];
    if (task.lifecycle != AYNI_TASK_WAITING || !task.operation_wait ||
        max_revision - task.revision < subscription_wake_revisions) {
        return false;
    }
    task.operation_wait.reset();
    task.lifecycle = AYNI_TASK_READY;
    progress(index, stamp);
    return true;
}

const catalog::CatalogId &TaskTable::step(uint32_t index, uint32_t step) const {
    return steps_of(index)[step];
}

const catalog::CatalogId &TaskTable::live_step(uint32_t index, uint32_t pos) const {
    const Task &task = tasks_[index];
    if (pos < task.step_count) {
        return steps_of(index)[pos];
    }
    return steps_of(index)[step_capacity_ - 1U - (pos - task.step_count)];
}

bool TaskTable::has_step(uint32_t index, ayni_string_v1 key) const {
    const Task &task = tasks_[index];
    const catalog::CatalogId *steps = steps_of(index);
    for (uint32_t step = 0; step < task.step_count; ++step) {
        if (steps[step].equals(key)) {
            return true;
        }
    }
    for (uint32_t step = 0; step < task.volatile_state.step_count; ++step) {
        if (steps[step_capacity_ - 1U - step].equals(key)) {
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

ayni_bytes_v1 TaskTable::live_checkpoint(uint32_t index) const {
    const Task &task = tasks_[index];
    const uint32_t size = task.volatile_state.checkpoint_dirty ? task.volatile_state.checkpoint_size
                                                               : task.checkpoint_size;
    return {checkpoints_ + static_cast<std::size_t>(task.slot) * checkpoint_max_bytes_, size};
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
        if (is_live(ended.lifecycle) ||
            (retention_.retains != nullptr && retention_.retains(retention_.context, ended))) {
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

// RUNNING owns a selected call; pause preserves it, and cancellation requests
// drainage before terminality. Policy/compatibility holds await an end because
// their inputs have not changed. Runtime holds instead survive user lifecycle
// transitions and clear only through an owner-managed ceiling raise.
bool TaskTable::allows(const Task &task, TaskChange change) {
    const ayni_task_lifecycle_t lifecycle = effective_lifecycle(task);
    switch (change) {
    case TaskChange::pause:
        // User pause must survive a later model-limit raise (C-RUN-003).
        return (task.lifecycle == AYNI_TASK_READY || task.lifecycle == AYNI_TASK_RUNNING ||
                task.lifecycle == AYNI_TASK_WAITING) &&
               (task.hold_status == AYNI_OK || task.call_unknown || waiting_for_resource(task)) &&
               task.recovery_hold_status == AYNI_OK;
    case TaskChange::complete:
    case TaskChange::fail:
        return lifecycle == AYNI_TASK_READY;
    case TaskChange::resume:
        // Resume may remove user pause, but cannot clear the model overlay.
        return task.lifecycle == AYNI_TASK_PAUSED &&
               (task.hold_status == AYNI_OK || waiting_for_resource(task)) && !task.call_unknown &&
               task.recovery_hold_status == AYNI_OK;
    case TaskChange::cancel:
    case TaskChange::expire:
        return lifecycle == AYNI_TASK_READY || lifecycle == AYNI_TASK_RUNNING ||
               lifecycle == AYNI_TASK_PAUSED || lifecycle == AYNI_TASK_WAITING;
    }
    return false;
}

bool TaskTable::at_largest_revision(const Task &task) {
    return max_revision - task.revision <= reserved_revisions(task);
}

bool TaskTable::has_revisions(const Task &task, uint32_t required,
                              uint32_t prospective_reserve) const {
    const auto existing = call_query_ ? call_query_(call_context_, task).revisions : 0U;
    const auto extra = existing > prospective_reserve ? existing : prospective_reserve;
    return max_revision - task.revision >= required &&
           max_revision - task.revision - required >= extra;
}
bool TaskTable::at_revision_limit(const Task &task) const {
    return !has_revisions(task, reserved_revisions(task) + 1U);
}
bool TaskTable::clear_settled_call_holds(Task &task) {
    const auto original_unknown = task.call_unknown;
    const auto original_hold = task.hold_status;
    const auto sources = call_query_ ? call_query_(call_context_, task) : CallConstraints{};
    if (!sources.unknown) {
        task.call_unknown = false;
    }
    if (waiting_for_resource(task) && !sources.queued) {
        task.hold_status = AYNI_OK;
        task.hold_reason = AYNI_REASON_NONE;
    }
    return task.call_unknown != original_unknown || task.hold_status != original_hold;
}
void TaskTable::settle_call_holds(uint32_t index, foundation::TransitionStamp stamp) {
    auto &task = tasks_[index];
    if (!is_live(task.lifecycle) || task.call.end != TaskCall::End::none) {
        return; // An ending foreground already blocks decisions and owns its final drain revision.
    }
    const bool held = still_held(task);
    if (!clear_settled_call_holds(task)) {
        return;
    }
    progress(index, stamp);
    if (held && !still_held(task)) {
        --held_count_;
    }
}

bool TaskTable::can_begin_with_calls(const Task &task) const {
    return can_begin_call(task) && has_revisions(task, call_acceptance_revisions);
}
bool TaskTable::can_begin_background_call(const Task &task) const {
    return can_begin_call(task) &&
           has_revisions(task, call_acceptance_revisions, background_call_revisions);
}
bool TaskTable::can_wait_with_calls(const Task &task) const {
    return can_wait_operation(task) && has_revisions(task, subscription_wait_revisions);
}

bool TaskTable::can_begin_call(const Task &task) {
    return task.call.id == 0U && task.volatile_state.call_id == 0U &&
           max_revision - task.revision >= call_acceptance_revisions;
}

bool TaskTable::can_begin_volatile_call(const Task &task) {
    return task.lifecycle == AYNI_TASK_READY && task.call.id == 0U &&
           task.volatile_state.call_id == 0U && !task.operation_wait && !is_held(task);
}

bool TaskTable::next_ready(uint64_t ready_before, uint32_t &out_index, ReadyFilter filter,
                           uint64_t volatile_before) const {
    uint32_t next = no_index;
    for (uint32_t index = 0; index < tasks_.size(); ++index) {
        const Task &task = tasks_[index];
        if (effective_lifecycle(task) != AYNI_TASK_READY || task.commit_seq >= ready_before ||
            task.volatile_state.generation >= volatile_before || at_revision_limit(task) ||
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
    if (!find(replaced_task_id, replaced) || is_live(tasks_[replaced].lifecycle) ||
        (retention_.retains != nullptr &&
         retention_.retains(retention_.context, tasks_[replaced]))) {
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
    // A last queued Call may settle the resource overlay without releasing another foreground.
    const bool resource_settled =
        waiting_for_resource(task) && state.hold_status == AYNI_OK &&
        state.hold_reason == AYNI_REASON_NONE && state.revision == task.revision + 1U &&
        state.lifecycle == task.lifecycle && state.cancel_revision == task.cancel_revision &&
        state.step_count == task.step_count && state.model_calls == task.model_calls &&
        state.call == task.call && state.operation_wait == task.operation_wait;
    // Only the independent Call overlay may change beneath an existing policy/recovery hold.
    const bool call_overlay =
        state.call_unknown != task.call_unknown && state.revision == task.revision + 1U &&
        state.lifecycle == task.lifecycle && state.cancel_revision == task.cancel_revision &&
        state.hold_status == task.hold_status && state.hold_reason == task.hold_reason &&
        state.step_count == task.step_count && state.model_calls == task.model_calls &&
        state.call == task.call && state.operation_wait == task.operation_wait;
    // Terminal history is immutable. Policy/compatibility holds only end;
    // the independent runtime overlay survives ordinary lifecycle changes.
    if (!is_live(task.lifecycle) ||
        (((task.hold_status != AYNI_OK && !waiting_for_resource(task)) ||
          task.recovery_hold_status != AYNI_OK) &&
         is_live(state.lifecycle) && !call_overlay && !resource_settled) ||
        // An ordinary live transition cannot clear the wait overlay; v12 owns that change.
        (waiting_for_resource(task) && is_live(state.lifecycle) &&
         (state.hold_status != task.hold_status || state.hold_reason != task.hold_reason) &&
         !resource_settled) ||
        !state.model_calls.valid() || state.model_calls.used < task.model_calls.used ||
        (task.created_transition_id != stamp.transition_id &&
         state.model_calls.limit != task.model_calls.limit) ||
        (task.model_calls.blocked && !state.model_calls.blocked) || !is_reached(state.lifecycle) ||
        state.revision < task.revision || state.step_count != task.step_count ||
        state.cancel_revision > state.revision || !state.call.valid(state.lifecycle) ||
        !valid_operation_wait(state.operation_wait, state.lifecycle, state.call.id) ||
        (state.cancel_revision != 0U) !=
            (state.lifecycle == AYNI_TASK_CANCELLED || (state.lifecycle == AYNI_TASK_CANCELLING &&
                                                        state.call.end == TaskCall::End::cancel)) ||
        (!held && state.hold_reason != AYNI_REASON_NONE) ||
        (held && state.lifecycle == AYNI_TASK_READY &&
         !(state.hold_status == AYNI_EXPIRED && state.hold_reason == AYNI_REASON_RESOURCE_WAIT))) {
        return Restore::corrupt;
    }
    if (still_held(task)) {
        --held_count_;
    }
    task.call_unknown = state.call_unknown;
    task.model_calls = state.model_calls;
    task.call = state.call;
    task.operation_wait = state.operation_wait;
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
    ++task.revision;
    task.commit_seq = stamp.commit_seq;
}

TaskTable::Restore TaskTable::restore_authority_state(uint32_t index, const TaskState &state,
                                                      foundation::TransitionStamp stamp) {
    const auto &task = tasks_[index];
    if (!is_live(task.lifecycle) || state.lifecycle != task.lifecycle ||
        state.revision != task.revision || state.cancel_revision != task.cancel_revision ||
        state.hold_status != task.hold_status || state.hold_reason != task.hold_reason ||
        state.step_count != task.step_count || state.call_unknown != task.call_unknown ||
        state.model_calls != task.model_calls || state.call != task.call ||
        state.operation_wait != task.operation_wait) {
        return Restore::corrupt;
    }
    authority_changed(index, stamp);
    return Restore::restored;
}

TaskTable::Restore TaskTable::restore_reauthorization_state(uint32_t index, const TaskState &state,
                                                            foundation::TransitionStamp stamp) {
    const auto &task = tasks_[index];
    if (!is_live(task.lifecycle) || at_revision_limit(task) ||
        stamp.commit_seq <= task.commit_seq || state.revision != task.revision + 1U ||
        state.lifecycle != task.lifecycle || state.cancel_revision != task.cancel_revision ||
        state.hold_status != task.hold_status || state.hold_reason != task.hold_reason ||
        state.step_count != task.step_count || state.call_unknown != task.call_unknown ||
        state.model_calls != task.model_calls || state.call != task.call ||
        state.operation_wait != task.operation_wait) {
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
    if (!is_live(task.lifecycle) || task.hold_status != AYNI_OK ||
        task.recovery_hold_status != AYNI_OK) {
        return;
    }
    // Runtime holds are releasable; retain missing-configuration evidence too.
    const bool held = still_held(task);
    task.recovery_hold_status = status;
    task.recovery_hold_reason = reason;
    if (!held) {
        ++held_count_;
    }
}

void TaskTable::reanchor(uint64_t now_us) {
    for (uint32_t index = 0; index < tasks_.size(); ++index) {
        tasks_[index].due_us = now_us;
    }
}

void TaskTable::clear() {
    tasks_.clear();
    held_count_ = 0;
    volatile_clock_ = 1;
}

bool TaskTable::is_live(ayni_task_lifecycle_t lifecycle) {
    return lifecycle == AYNI_TASK_READY || lifecycle == AYNI_TASK_RUNNING ||
           lifecycle == AYNI_TASK_PAUSED || lifecycle == AYNI_TASK_CANCELLING ||
           lifecycle == AYNI_TASK_WAITING;
}

} // namespace ayni::work
