// Reauthorization uses the existing command/transition coordinator. All
// fallible admission precedes one Goal/direct-live-Task binding publication.
#include "kernel/intent/authority_intent.h"

namespace ayni::kernel {
namespace {
// Borrowed immutable ceiling and replacement selections for one synchronous call.
struct ReauthorizationPlan {
    uint32_t goal_index = 0;            // Valid only after exact identity/CAS admission.
    work::GoalAuthorityRecord previous; // Original Scope views; never re-exported.
    std::array<work::AuthorityReference, AYNI_GOAL_REQUIREMENT_CAPACITY_MAX> references{};
};

ayni_status_t refuse(ayni_status_t status, ayni_reason_t reason, IntentFailure &failure) {
    failure.reason = reason;
    return status;
}

// Goal owns revision/lifecycle; immutable echo and authorization CAS each
// prevent a management decision from silently changing a different fact.
ayni_status_t admit_goal(const WorkModel &model, const Intent &intent, uint64_t now_us,
                         uint64_t durable_seq, ReauthorizationPlan &plan, IntentFailure &failure) {
    const auto &request = intent.reauthorization;
    if (!model.goals.find(request.goal_id, plan.goal_index)) {
        return refuse(AYNI_NOT_FOUND, AYNI_REASON_GOAL_UNKNOWN, failure);
    }
    const auto &goal = model.goals.at(plan.goal_index);
    failure.object = {0, goal.goal_id, goal.revision, AYNI_OBJECT_GOAL};
    switch (model.goals.check_reauthorization(plan.goal_index, intent.expected_revision, now_us)) {
    case work::GoalTable::ChangeCheck::revision_conflict:
        return refuse(AYNI_REVISION_CONFLICT, AYNI_REASON_GOAL_REVISION, failure);
    case work::GoalTable::ChangeCheck::invalid_state:
        return refuse(AYNI_INVALID_STATE, AYNI_REASON_GOAL_STATE, failure);
    case work::GoalTable::ChangeCheck::revision_exhausted:
        return refuse(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_REVISION_EXHAUSTED, failure);
    case work::GoalTable::ChangeCheck::allowed:
        break;
    }
    if (!model.goal_authority.read(plan.goal_index, goal.goal_id, plan.previous) ||
        plan.previous.phase == work::GoalAuthorityPhase::proposed) {
        return refuse(AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_MISSING, failure);
    }
    if (plan.previous.stamp.commit_seq != request.expected_authority_commit_seq) {
        return refuse(AYNI_REVISION_CONFLICT, AYNI_REASON_GOAL_AUTHORITY_REVISION, failure);
    }
    if (!matches_goal_creation(request.original, goal, model.goals.parameters(plan.goal_index))) {
        return refuse(AYNI_DENIED, AYNI_REASON_GOAL_REAUTHORIZATION_CHANGED, failure);
    }
    const auto current = model.check_goal_authority(plan.goal_index, now_us, durable_seq);
    if (plan.previous.phase == work::GoalAuthorityPhase::authorized &&
        (current.status == AYNI_OK || current.status == AYNI_BUSY)) {
        return refuse(AYNI_INVALID_STATE, AYNI_REASON_GOAL_STATE, failure);
    }
    return AYNI_OK;
}

authority::GrantCheck check_context(const WorkModel &model, const ReauthorizationPlan &plan,
                                    uint64_t now_us, uint64_t durable_seq) {
    const auto &goal = model.goals.at(plan.goal_index);
    if (!work::AgentTable::usable(model.agents.find_instance(goal.owner_instance_id))) {
        return {AYNI_DENIED, AYNI_REASON_GRANT_SUBJECT};
    }
    if (!model.parent_covers_authority(goal.parent_goal_id, plan.previous)) {
        return {AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_PARENT};
    }
    const auto parent = model.check_parent_authority(goal.parent_index, now_us, durable_seq);
    if (parent.status != AYNI_OK) {
        return parent;
    }
    return model.check_reauthorization_tasks(plan.goal_index);
}

// Each requirement chooses one complete existing durable Grant. Dedicated
// roots outrank generic authority; ID breaks ties within the same category.
authority::GrantCheck select_references(const WorkModel &model, ReauthorizationPlan &plan,
                                        uint64_t now_us, uint64_t durable_seq) {
    const auto &goal = model.goals.at(plan.goal_index);
    for (uint32_t at = 0; at < plan.previous.count; ++at) {
        const auto &need = plan.previous.requirements[at];
        const auto entry = model.capabilities.find(
            {need.capability_id.data(), need.capability_id.size()}, need.capability_version);
        if (entry.kind != catalog::CapabilityRegistry::Lookup::Kind::found ||
            !model.capabilities.matches_domain(entry.index, need.source_domain)) {
            return {AYNI_DENIED, AYNI_REASON_GRANT_CATALOG_CHANGED};
        }
        std::optional<uint32_t> selected;
        for (uint32_t index = 0; index < model.grants.size(); ++index) {
            const auto &grant = model.grants.at(index);
            if (grant.subject_instance_id != goal.owner_instance_id ||
                !model.grants.covers_saved_scope(index, need.capability_id, need.capability_version,
                                                 need.source_domain, need.scope) ||
                model.check_grant(index, now_us, durable_seq, goal.goal_id).status != AYNI_OK) {
                continue;
            }
            if (!selected ||
                (grant.bound_goal_id != 0U && model.grants.at(*selected).bound_goal_id == 0U) ||
                ((grant.bound_goal_id != 0U) == (model.grants.at(*selected).bound_goal_id != 0U) &&
                 grant.grant_id < model.grants.at(*selected).grant_id)) {
                selected = index;
            }
        }
        if (!selected) {
            return {AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_MISSING};
        }
        const auto &grant = model.grants.at(*selected);
        plan.references[at] = {grant.grant_id, grant.revision};
    }
    return {AYNI_OK, AYNI_REASON_NONE};
}

// Every reference, revision and storage reservation is fixed. Sample ingress
// only now; publication contains no caller callback, allocation or new choice.
bool publish(WorkModel &model, const ReauthorizationPlan &plan, uint64_t now_us,
             InputResumeSampler input, IntentOutcome &outcome) {
    const auto boundary = input.sample();
    if (boundary.incarnation == 0U || !work::valid_input_resume_boundary(boundary)) {
        return false;
    }
    const auto &goal = model.goals.at(plan.goal_index);
    work::BindingReference parent{};
    if (goal.parent_index != work::GoalTable::no_parent) {
        parent = {goal.parent_goal_id,
                  model.goal_authority.at(goal.parent_index).binding.established_seq};
    }
    const auto stamp = model.transitions.next();
    model.release_goal_references(plan.previous, stamp.commit_seq);
    model.goal_authority.bind(plan.goal_index, plan.references, stamp, parent, boundary);
    model.goals.reauthorize(plan.goal_index, now_us, stamp);
    for (uint32_t at = 0; at < model.tasks.size(); ++at) {
        const auto &task = model.tasks.at(at);
        if (task.goal_id != goal.goal_id || !work::TaskTable::is_live(task.lifecycle)) {
            continue;
        }
        auto record = model.task_authority.at(task.slot);
        model.release_task_references(record, stamp.commit_seq);
        record.ceiling.commit_seq = stamp.commit_seq;
        record.stamp = stamp;
        record.phase = work::TaskAuthorityPhase::authorized;
        record.references = plan.references;
        if (model.task_authority.restore_validated(task.slot, record) != AYNI_OK) {
            return false; // The unchanged fully preflighted cohort cannot refuse publication.
        }
        model.tasks.reauthorize(at, stamp);
    }
    outcome.object = {0, goal.goal_id, goal.revision, AYNI_OBJECT_GOAL};
    outcome.commit_seq = stamp.commit_seq;
    outcome.authorization = copy_work_authority_result(model.goal_authority.at(plan.goal_index));
    outcome.due_work = true;
    return true;
}
} // namespace

ayni_status_t reauthorize_goal(WorkModel &model, const Intent &intent, uint64_t now_us,
                               uint64_t durable_seq, InputResumeSampler input,
                               IntentOutcome &outcome, IntentFailure &failure) {
    ReauthorizationPlan plan{};
    const auto admitted = admit_goal(model, intent, now_us, durable_seq, plan, failure);
    if (admitted != AYNI_OK) {
        return admitted;
    }
    auto checked = check_context(model, plan, now_us, durable_seq);
    if (checked.status == AYNI_OK) {
        checked = select_references(model, plan, now_us, durable_seq);
    }
    if (checked.status != AYNI_OK) {
        return refuse(checked.status, checked.reason, failure);
    }
    return publish(model, plan, now_us, input, outcome) ? AYNI_OK : AYNI_INTERNAL;
}
} // namespace ayni::kernel
