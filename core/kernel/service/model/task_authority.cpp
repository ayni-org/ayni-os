// Cross-table Task admission and reference handoff (ADR-0038). Tables keep
// representation/history checks; this module coordinates work with Grants.
#include "kernel/service/model/work_model.h"

namespace ayni::kernel {

// The complete direct live cohort must still name this exact old binding.
// Missing/partial old admissions are refused, never repaired as legacy data.
authority::GrantCheck WorkModel::check_reauthorization_tasks(uint32_t goal_index) const {
    const auto &goal = goals.at(goal_index);
    const auto previous = goal_authority.at(goal_index);
    for (uint32_t at = 0; at < tasks.size(); ++at) {
        const auto &task = tasks.at(at);
        if (task.goal_id != goal.goal_id || !work::TaskTable::is_live(task.lifecycle)) {
            continue;
        }
        if (work::TaskTable::at_largest_revision(task)) {
            return {AYNI_CAPACITY_EXCEEDED, AYNI_REASON_REVISION_EXHAUSTED};
        }
        work::TaskAuthorityRecord record{};
        if (!task_authority.read(task.slot, task.task_id, record) ||
            !matches_task_ceiling(record, goal_index) ||
            ((record.phase == work::TaskAuthorityPhase::paused) !=
             (previous.phase == work::GoalAuthorityPhase::paused))) {
            return {AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_MISSING};
        }
    }
    return {AYNI_OK, AYNI_REASON_NONE};
}

// Compare the retained generation and selections exactly; a covering Grant
// cannot replace the admission this Task inherited.
bool WorkModel::matches_task_ceiling(const work::TaskAuthorityRecord &record,
                                     uint32_t goal_index) const {
    const auto &goal = goals.at(goal_index);
    if (!goal_authority.contains(goal_index, goal.goal_id)) {
        return false;
    }
    const auto ceiling = goal_authority.at(goal_index);
    if (record.ceiling.goal_id != goal.goal_id ||
        record.ceiling.commit_seq != ceiling.binding.established_seq ||
        record.count != ceiling.count) {
        return false;
    }
    for (uint32_t at = 0; at < record.count; ++at) {
        const auto &reference = record.references[at];
        const auto &selected = ceiling.requirements[at].dependency;
        if (reference.grant_id != selected.grant_id || reference.revision != selected.revision) {
            return false;
        }
    }
    return true;
}

// Require the Task's own admission, then reuse the Goal's checked chain only
// when every inherited identity matches; missing evidence never means zero needs.
authority::GrantCheck WorkModel::check_task_authority(uint32_t index, uint64_t now_us,
                                                      uint64_t durable_seq) const {
    const auto &task = tasks.at(index);
    work::TaskAuthorityRecord saved{};
    if (!task_authority.read(task.slot, task.task_id, saved) ||
        saved.ceiling.goal_id != task.goal_id) {
        return {AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_MISSING};
    }
    if (saved.phase != work::TaskAuthorityPhase::authorized) {
        return {AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_PAUSED};
    }
    const auto goal = check_goal_authority(task.goal_index, now_us, durable_seq);
    if (goal.status != AYNI_OK) {
        return goal;
    }
    if (!matches_task_ceiling(saved, task.goal_index)) {
        return {AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_MISSING};
    }
    // Same immutable references reuse the Goal's successful chain check;
    // checking every identical Grant again would duplicate admission.
    return {AYNI_OK, AYNI_REASON_NONE};
}

// Derivation has reserved a Task slot and an unchanged complete Goal ceiling.
// Release a replaced terminal Task only after every fallible admission passed.
bool WorkModel::inherit_task_authority(uint64_t task_id, foundation::TransitionStamp stamp) {
    uint32_t index = 0;
    if (!tasks.find(task_id, index)) {
        return false;
    }
    const auto &task = tasks.at(index);
    calls.retire_task(task.replaced_task_id);
    const auto ceiling = goal_authority.at(task.goal_index);
    work::TaskAuthorityRecord record{};
    record.task_id = task.task_id;
    record.ceiling = {task.goal_id, ceiling.binding.established_seq};
    record.stamp = stamp;
    record.count = ceiling.count;
    for (uint32_t at = 0; at < ceiling.count; ++at) {
        record.references[at] = ceiling.requirements[at].dependency;
    }
    work::TaskAuthorityRecord replaced{};
    if (task_authority.read(task.slot, task.replaced_task_id, replaced)) {
        release_task_references(replaced, stamp.commit_seq);
    }
    // Configured Task and Goal requirement bounds are identical. A failure
    // here is an internal defect, never a partially accepted derivation.
    return task_authority.install_validated(task.slot, record) == AYNI_OK;
}

void WorkModel::release_task_references(const work::TaskAuthorityRecord &record,
                                        uint64_t commit_seq) {
    for (uint32_t at = 0; at < record.count; ++at) {
        grants.release_reference(record.references[at].grant_id, commit_seq);
    }
}

void WorkModel::clear_goal_task_authority(uint32_t goal_index, uint64_t commit_seq) {
    for (uint32_t index = 0; index < tasks.size(); ++index) {
        const auto &task = tasks.at(index);
        work::TaskAuthorityRecord record{};
        if (task.goal_index != goal_index ||
            !task_authority.read(task.slot, task.task_id, record)) {
            continue;
        }
        calls.retire_task(task.task_id);
        release_task_references(record, commit_seq);
        task_authority.clear(task.slot);
    }
}

} // namespace ayni::kernel
