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
        if (tasks.at_revision_limit(task)) {
            return {AYNI_CAPACITY_EXCEEDED, AYNI_REASON_REVISION_EXHAUSTED};
        }
        work::TaskAuthorityRecord record{};
        if (!task_authority.read(task.slot, task.task_id, record) ||
            !matches_task_ceiling(record, goal_index) ||
            (previous.phase == work::GoalAuthorityPhase::paused &&
             record.phase != work::TaskAuthorityPhase::paused)) {
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
    // Mandatory references reuse the Goal check; optional bindings have their
    // own immutable Call/Grant anchors and selected chain (ADR-0085).
    for (uint32_t at = 0; at < saved.tool_count; ++at) {
        const auto check = check_tool_authority(index, saved.tools[at], now_us, durable_seq);
        if (check.status != AYNI_OK) {
            return check;
        }
    }
    return {AYNI_OK, AYNI_REASON_NONE};
}

// Derivation has reserved a Task slot and an unchanged complete Goal ceiling.
// Release a replaced terminal Task only after every fallible admission passed.
bool WorkModel::inherit_task_metadata(uint64_t task_id, foundation::TransitionStamp stamp) {
    uint32_t index = 0;
    if (!tasks.find(task_id, index)) {
        return false;
    }
    const auto &task = tasks.at(index);
    clear_content_provenance(work::ProvenanceKind::task, task.slot, task.replaced_task_id,
                             stamp.commit_seq);
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
    if (retire_task_calls(task.replaced_task_id, {ids.boot_epoch(), stamp}, stamp.commit_seq) !=
        AYNI_OK) {
        return false;
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
    for (uint32_t at = 0; at < record.tool_count; ++at) {
        grants.release_reference(record.tools[at].selected.grant_id, commit_seq);
        uint32_t source_call = 0;
        if (calls.find(record.tools[at].source_call_id, source_call)) {
            grants.release_reference(calls.identity(source_call).authorization.grant_id,
                                     commit_seq);
        }
    }
}

namespace {
// Preflight the entire handoff before any table or body-reference mutation (ADR-0115).
ayni_status_t check_task_handoff(const WorkModel &model, uint64_t task_id,
                                 work::ActionStamp stamp) {
    if (!model.calls.can_retire_task(task_id, UINT64_MAX)) {
        return AYNI_INVALID_STATE;
    }
    const auto count = model.calls.unresolved_durable_count(task_id);
    if (!model.unresolved_ops.has_room(count)) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    for (uint32_t at = 0; at < model.calls.capacity(); ++at) {
        if (!model.calls.occupied(at) || model.calls.identity(at).task_id != task_id ||
            model.calls.state(at).ephemeral || !model.calls.unresolved(at)) {
            continue;
        }
        const auto status = model.unresolved_ops.check_admit_from_call(
            model.calls.identity(at), model.calls.contract(at), model.calls.state(at),
            *model.calls.action(at), stamp);
        if (status != AYNI_OK) {
            return status;
        }
    }
    return AYNI_OK;
}
// Owner-thread commit phase after the complete cohort's capacity and evidence checks.
void retire_checked_task_calls(WorkModel &model, uint64_t task_id, work::ActionStamp stamp,
                               uint64_t release_seq) {
    for (uint32_t at = 0; at < model.calls.capacity(); ++at) {
        if (model.calls.occupied(at) && model.calls.identity(at).task_id == task_id &&
            !model.calls.state(at).ephemeral && model.calls.unresolved(at)) {
            model.unresolved_ops.admit_checked_call(model.calls.identity(at),
                                                    model.calls.contract(at), model.calls.state(at),
                                                    *model.calls.action(at), stamp);
        }
    }
    model.calls.retire_task(task_id, release_seq);
}
} // namespace
ayni_status_t WorkModel::retire_task_calls(uint64_t task_id, work::ActionStamp stamp,
                                           uint64_t release_seq) {
    if (task_id == 0) {
        return AYNI_OK;
    }
    const auto status = check_task_handoff(*this, task_id, stamp);
    if (status != AYNI_OK) {
        return status;
    }
    retire_checked_task_calls(*this, task_id, stamp, release_seq);
    return AYNI_OK;
}

ayni_status_t WorkModel::retire_goal_task_metadata(uint32_t goal_index, work::ActionStamp stamp,
                                                   uint64_t release_seq) {
    const auto commit_seq = stamp.transition.commit_seq;
    uint32_t count = 0;
    uint32_t body_refs = 0;
    for (uint32_t at = 0; at < tasks.size(); ++at) {
        const auto &task = tasks.at(at);
        if (task.goal_index != goal_index) {
            continue;
        }
        count += calls.unresolved_durable_count(task.task_id);
        body_refs += calls.task_body_refs(task.task_id);
        const auto status = check_task_handoff(*this, task.task_id, stamp);
        if (status != AYNI_OK) {
            return status;
        }
    }
    if (!unresolved_ops.has_room(count) || !calls.release_fits(body_refs)) {
        return AYNI_CAPACITY_EXCEEDED;
    }

    for (uint32_t index = 0; index < tasks.size(); ++index) {
        const auto &task = tasks.at(index);
        work::TaskAuthorityRecord record{};
        if (task.goal_index != goal_index) {
            continue;
        }
        clear_content_provenance(work::ProvenanceKind::task, task.slot, task.task_id, commit_seq);
        if (task_authority.read(task.slot, task.task_id, record)) {
            release_task_references(record, commit_seq);
        }
        retire_checked_task_calls(*this, task.task_id, stamp, release_seq);
        task_authority.clear(task.slot);
    }
    return AYNI_OK;
}

} // namespace ayni::kernel
