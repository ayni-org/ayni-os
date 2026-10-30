#include "work/task/task_authority.h"

#include "foundation/kernel_id.h"
#include <new>

namespace ayni::work {
namespace {
constexpr std::size_t tool_regions =
    2; // Live references and owner-command reauthorization scratch.
} // namespace

// Only complete Task admissions are representable. Current usability and
// Goal/Grant existence are separate cross-table checks, not codec semantics.
bool valid_task_authority_record(const TaskAuthorityRecord &record) {
    if (!foundation::is_kernel_id(record.task_id) || !valid_binding_reference(record.ceiling) ||
        record.ceiling.goal_id == 0U || record.ceiling.goal_id == record.task_id ||
        record.stamp.transition_id == 0U || record.ceiling.commit_seq > record.stamp.commit_seq ||
        (record.phase != TaskAuthorityPhase::authorized &&
         record.phase != TaskAuthorityPhase::paused) ||
        record.count > AYNI_GOAL_REQUIREMENT_CAPACITY_MAX ||
        record.tool_count > AYNI_TASK_STEP_CAPACITY_MAX) {
        return false;
    }
    for (uint32_t index = 0; index < record.count; ++index) {
        if (!valid_selected_authority_reference(record.references[index])) {
            return false;
        }
    }
    for (uint32_t at = 0; at < record.tool_count; ++at) {
        const auto &tool = record.tools[at];
        if (!foundation::is_kernel_id(tool.source_call_id) ||
            tool.source_call_id == record.task_id ||
            !valid_selected_authority_reference(tool.selected) ||
            (at && record.tools[at - 1U].source_call_id >= tool.source_call_id)) {
            return false;
        }
    }
    return true;
}

std::size_t TaskAuthorityTable::storage_size(Limits limits) {
    return sizeof(Slot) * limits.tasks +
           sizeof(AuthorityReference) * limits.tasks * limits.requirements +
           tool_regions * sizeof(TaskToolReference) * limits.tasks * limits.tools;
}

TaskAuthorityTable::TaskAuthorityTable(void *storage, Limits limits)
    : slots_(static_cast<Slot *>(storage)),
      references_(reinterpret_cast<AuthorityReference *>(slots_ + limits.tasks)), limits_(limits) {
    tools_ = reinterpret_cast<TaskToolReference *>(
        references_ + static_cast<std::size_t>(limits.tasks) * limits.requirements);
    for (std::size_t at = 0;
         at < tool_regions * static_cast<std::size_t>(limits.tasks) * limits.tools; ++at) {
        new (tools_ + at) TaskToolReference{};
    }
    for (uint32_t slot = 0; slot < limits.tasks; ++slot) {
        new (slots_ + slot) Slot{};
    }
    const auto count = static_cast<std::size_t>(limits.tasks) * limits.requirements;
    for (std::size_t index = 0; index < count; ++index) {
        new (references_ + index) AuthorityReference{};
    }
}

AuthorityReference *TaskAuthorityTable::references_at(uint32_t slot) const {
    return references_ + static_cast<std::size_t>(slot) * limits_.requirements;
}

ayni_status_t TaskAuthorityTable::install_validated(uint32_t slot,
                                                    const TaskAuthorityRecord &record) {
    if (slot >= limits_.tasks || record.count > limits_.requirements ||
        record.tool_count > limits_.tools) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto *references = references_at(slot);
    for (uint32_t index = 0; index < record.count; ++index) {
        references[index] = record.references[index];
    }
    for (uint32_t at = 0; at < record.tool_count; ++at) {
        tools_at(slot)[at] = record.tools[at];
    }
    slots_[slot] = {record.task_id, record.ceiling, record.stamp,
                    record.phase,   record.count,   record.tool_count};
    return AYNI_OK;
}

bool TaskAuthorityTable::same_references(uint32_t slot, const TaskAuthorityRecord &record) const {
    const auto *references = references_at(slot);
    for (uint32_t index = 0; index < record.count; ++index) {
        if (references[index].grant_id != record.references[index].grant_id ||
            references[index].revision != record.references[index].revision) {
            return false;
        }
    }
    for (uint32_t at = 0; at < slots_[slot].tool_count; ++at) {
        const auto &before = tools_at(slot)[at].selected;
        const auto &after = record.tools[at].selected;
        if (before.grant_id != after.grant_id || before.revision != after.revision) {
            return false;
        }
    }
    return true;
}

// Shape is already validated. These history rules prevent an ordinary pause
// from substituting references or resuming the same revoked binding.
ayni_status_t TaskAuthorityTable::restore_validated(uint32_t slot,
                                                    const TaskAuthorityRecord &record) {
    if (slot >= limits_.tasks || record.count > limits_.requirements ||
        record.tool_count > limits_.tools) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    const auto &previous = slots_[slot];
    if (previous.task_id && (previous.tool_count > record.tool_count ||
                             (previous.tool_count != record.tool_count &&
                              (previous.phase != TaskAuthorityPhase::authorized ||
                               record.phase != TaskAuthorityPhase::authorized)))) {
        return AYNI_INVALID_ARGUMENT;
    }
    for (uint32_t at = 0; previous.task_id && at < previous.tool_count; ++at) {
        if (tools_at(slot)[at].source_call_id != record.tools[at].source_call_id) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    if (previous.task_id != 0U &&
        (previous.task_id != record.task_id || previous.ceiling.goal_id != record.ceiling.goal_id ||
         previous.count != record.count || previous.stamp.commit_seq >= record.stamp.commit_seq ||
         previous.stamp.transition_id >= record.stamp.transition_id ||
         previous.ceiling.commit_seq > record.ceiling.commit_seq ||
         (previous.ceiling.commit_seq == record.ceiling.commit_seq &&
          (!same_references(slot, record) || (previous.phase == TaskAuthorityPhase::paused &&
                                              record.phase == TaskAuthorityPhase::authorized))))) {
        return AYNI_INVALID_ARGUMENT;
    }
    return install_validated(slot, record);
}

bool TaskAuthorityTable::pause(uint32_t slot, foundation::TransitionStamp stamp) {
    if (slots_[slot].phase == TaskAuthorityPhase::authorized) {
        slots_[slot].phase = TaskAuthorityPhase::paused;
        slots_[slot].stamp = stamp;
        return true;
    }
    return false;
}

bool TaskAuthorityTable::contains(uint32_t slot, uint64_t task_id) const {
    return slot < limits_.tasks && task_id != 0U && slots_[slot].task_id == task_id;
}

bool TaskAuthorityTable::read(uint32_t slot, uint64_t task_id, TaskAuthorityRecord &out) const {
    if (!contains(slot, task_id)) {
        return false;
    }
    out = at(slot);
    return true;
}

TaskAuthorityRecord TaskAuthorityTable::at(uint32_t slot) const {
    const auto &saved = slots_[slot];
    TaskAuthorityRecord record{saved.task_id, saved.ceiling, saved.stamp, saved.phase, saved.count};
    const auto *references = references_at(slot);
    for (uint32_t index = 0; index < saved.count; ++index) {
        record.references[index] = references[index];
    }
    record.tool_count = saved.tool_count;
    for (uint32_t at = 0; at < saved.tool_count; ++at) {
        record.tools[at] = tools_at(slot)[at];
    }
    return record;
}

TaskToolReference *TaskAuthorityTable::tools_at(uint32_t slot) const {
    return tools_ + static_cast<std::size_t>(slot) * limits_.tools;
}
TaskToolReference *TaskAuthorityTable::staged_at(uint32_t slot) const {
    return tools_ + static_cast<std::size_t>(limits_.tasks + slot) * limits_.tools;
}
bool TaskAuthorityTable::can_bind_tool(uint32_t slot) const {
    return slot < limits_.tasks && slots_[slot].task_id &&
           slots_[slot].phase == TaskAuthorityPhase::authorized &&
           slots_[slot].tool_count < limits_.tools;
}
void TaskAuthorityTable::bind_tool_validated(uint32_t slot, TaskToolReference reference,
                                             foundation::TransitionStamp stamp) {
    tools_at(slot)[slots_[slot].tool_count++] = reference;
    slots_[slot].stamp = stamp;
}
void TaskAuthorityTable::stage_tool_references(
    uint32_t slot, const std::array<AuthorityReference, AYNI_TASK_STEP_CAPACITY_MAX> &selected) {
    for (uint32_t at = 0; at < slots_[slot].tool_count; ++at) {
        staged_at(slot)[at] = {tools_at(slot)[at].source_call_id, selected[at]};
    }
}
void TaskAuthorityTable::copy_staged_tools(uint32_t slot, TaskAuthorityRecord &record) const {
    record.tool_count = slots_[slot].tool_count;
    for (uint32_t at = 0; at < record.tool_count; ++at) {
        record.tools[at] = staged_at(slot)[at];
    }
}

void TaskAuthorityTable::clear(uint32_t slot) {
    if (slot < limits_.tasks) {
        slots_[slot] = {};
    }
}

void TaskAuthorityTable::clear_all() {
    for (uint32_t slot = 0; slot < limits_.tasks; ++slot) {
        clear(slot);
    }
}

} // namespace ayni::work
