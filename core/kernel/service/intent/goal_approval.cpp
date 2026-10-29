#include "kernel/service/intent/authority_intent.h"

namespace ayni::kernel {
namespace {

// One synchronous admission plan; views refer to the immutable proposal row.
// No Grant, ID or work state is published until every reservation succeeds.
struct ApprovalPlan {
    uint32_t goal_index = 0;          // Valid only after goal admission succeeds.
    work::GoalAuthorityRecord record; // Original ceiling and dependency views.
    std::array<authority::GrantTable::Admission, AYNI_GOAL_REQUIREMENT_CAPACITY_MAX> placements{};
};

ayni_status_t refuse(ayni_status_t status, ayni_reason_t reason, IntentFailure &failure,
                     ayni_object_ref_v1 object = {}) {
    failure = {reason, object};
    return status;
}

// Revision and lifecycle belong to GoalTable; immutable request equality and
// the original missing mask bind management's decision to this exact proposal.
ayni_status_t admit_goal(const WorkModel &model, const Intent &intent, uint64_t now_us,
                         ApprovalPlan &plan, IntentFailure &failure) {
    const auto &request = intent.approval;
    if (!model.goals.find(request.goal_id, plan.goal_index)) {
        return refuse(AYNI_NOT_FOUND, AYNI_REASON_GOAL_UNKNOWN, failure,
                      {0, request.goal_id, 0, AYNI_OBJECT_GOAL});
    }
    const auto &goal = model.goals.at(plan.goal_index);
    const ayni_object_ref_v1 object{0, goal.goal_id, goal.revision, AYNI_OBJECT_GOAL};
    switch (model.goals.check_approval(plan.goal_index, intent.expected_revision, now_us)) {
    case work::GoalTable::ChangeCheck::revision_conflict:
        return refuse(AYNI_REVISION_CONFLICT, AYNI_REASON_GOAL_REVISION, failure, object);
    case work::GoalTable::ChangeCheck::invalid_state:
        return refuse(AYNI_INVALID_STATE, AYNI_REASON_GOAL_STATE, failure, object);
    case work::GoalTable::ChangeCheck::revision_exhausted:
        return refuse(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_REVISION_EXHAUSTED, failure, object);
    case work::GoalTable::ChangeCheck::allowed:
        break;
    }
    if (!model.goal_authority.read(plan.goal_index, goal.goal_id, plan.record) ||
        plan.record.phase != work::GoalAuthorityPhase::proposed ||
        !matches_goal_creation(request.original, goal, model.goals.parameters(plan.goal_index),
                               &model.calendars.at(plan.goal_index).rule) ||
        request.missing_mask != work::missing_requirement_mask(plan.record)) {
        return refuse(AYNI_DENIED, AYNI_REASON_GOAL_APPROVAL_CHANGED, failure, object);
    }
    return AYNI_OK;
}

authority::NewGrant grant_for(const GoalApproval &request, const work::GoalRequirement &need,
                              const authority::Session &session) {
    return {request.original.owner_instance_id,
            0,
            session.principal_id,
            session.binding_revision,
            need.capability_id,
            need.capability_version,
            need.source_domain,
            nullptr,
            request.lifetime,
            request.duration_us,
            0,
            request.goal_id,
            need.business_contract};
}

// Approval does not repair a changed parent or old dependency. It also cannot
// interpret a saved requirement using a changed same-version catalog domain.
authority::GrantCheck check_context(const WorkModel &model, const ApprovalPlan &plan,
                                    uint64_t now_us, uint64_t durable_seq) {
    const auto &goal = model.goals.at(plan.goal_index);
    if (!work::AgentTable::usable(model.agents.find_instance(goal.owner_instance_id))) {
        return {AYNI_DENIED, AYNI_REASON_GRANT_SUBJECT};
    }
    const auto parent = model.check_parent_authority(goal.parent_index, now_us, durable_seq);
    if (parent.status != AYNI_OK) {
        return parent;
    }
    for (uint32_t index = 0; index < plan.record.count; ++index) {
        const auto &need = plan.record.requirements[index];
        const auto entry = model.catalogs.capabilities.find(
            {need.capability_id.data(), need.capability_id.size()}, need.capability_version);
        if (entry.kind != catalog::CapabilityRegistry::Lookup::Kind::found ||
            !model.catalogs.capabilities.matches_domain(entry.index, need.source_domain) ||
            !model.catalogs.capabilities.matches_business(entry.index, need.business_contract)) {
            return {AYNI_DENIED, AYNI_REASON_GRANT_CATALOG_CHANGED};
        }
        if (need.dependency.grant_id != 0U) {
            const auto check = model.check_requirement(need, now_us, durable_seq, goal.goal_id);
            if (check.status != AYNI_OK) {
                return check;
            }
        }
    }
    return {AYNI_OK, AYNI_REASON_NONE};
}

// Reserve all slots and IDs against the same unchanged model. A refusal
// discards this local plan before any dependency or Grant can be published.
authority::GrantCheck reserve_batch(WorkModel &model, const Intent &intent,
                                    const authority::Session &session, uint64_t now_us,
                                    uint64_t durable_seq, ApprovalPlan &plan) {
    authority::GrantTable::BatchPlacement placement{model.grant_reclaim_exclusions(durable_seq)};
    uint32_t needed = 0;
    for (uint32_t index = 0; index < plan.record.count; ++index) {
        const auto &need = plan.record.requirements[index];
        if (need.dependency.grant_id != 0U) {
            continue; // ADR-0037: retain the original dependency, even if another Grant exists.
        }
        const auto check = model.grants.check_goal_create(
            grant_for(intent.approval, need, session), need.scope,
            {model.ids.boot_epoch(), now_us, durable_seq, intent.approval.goal_id}, placement,
            plan.placements[index]);
        if (check.status != AYNI_OK) {
            return check;
        }
        ++needed;
    }
    return model.ids.available(needed)
               ? authority::GrantCheck{AYNI_OK, AYNI_REASON_NONE}
               : authority::GrantCheck{AYNI_CAPACITY_EXCEEDED, AYNI_REASON_ID_EXHAUSTED};
}

// Only admitted facts reach publication. Exact command space and storage were
// reserved by apply_intent; this function performs no callback or new admission.
bool publish_batch(WorkModel &model, const Intent &intent, const authority::Session &session,
                   const ApprovalPlan &plan, uint64_t now_us, IntentOutcome &outcome) {
    std::array<work::GoalGrantReference, AYNI_GOAL_REQUIREMENT_CAPACITY_MAX> references{};
    const auto stamp = model.transitions.next();
    for (uint32_t index = 0; index < plan.record.count; ++index) {
        const auto &need = plan.record.requirements[index];
        references[index] = need.dependency;
        if (references[index].grant_id != 0U) {
            continue;
        }
        uint64_t id = 0;
        uint32_t grant_index = 0;
        if (!model.ids.allocate(id) ||
            !model.grants.insert_goal(grant_for(intent.approval, need, session), need.scope,
                                      plan.placements[index], id, stamp, grant_index)) {
            return false; // An unchanged successful plan makes failure an internal defect.
        }
        references[index] = {id, model.grants.at(grant_index).revision};
    }
    work::BindingReference parent{};
    const auto parent_index = model.goals.at(plan.goal_index).parent_index;
    if (parent_index != work::GoalTable::no_parent) {
        parent = {model.goals.at(parent_index).goal_id,
                  model.goal_authority.at(parent_index).binding.established_seq};
    }
    model.goal_authority.bind(plan.goal_index, references, stamp, parent);
    model.goals.approve(plan.goal_index, now_us, stamp);
    const auto &goal = model.goals.at(plan.goal_index);
    outcome.object = {0, goal.goal_id, goal.revision, AYNI_OBJECT_GOAL};
    outcome.commit_seq = stamp.commit_seq;
    outcome.due_work = true;
    outcome.authorization = copy_work_authority_result(model.goal_authority.at(plan.goal_index));
    return true;
}

} // namespace

ayni_status_t approve_goal(WorkModel &model, const Intent &intent,
                           const authority::Session &session, uint64_t now_us, uint64_t durable_seq,
                           IntentOutcome &outcome, IntentFailure &failure) {
    ApprovalPlan plan{};
    const auto admitted = admit_goal(model, intent, now_us, plan, failure);
    if (admitted != AYNI_OK) {
        return admitted;
    }
    const auto context = check_context(model, plan, now_us, durable_seq);
    if (context.status != AYNI_OK) {
        return refuse(context.status, context.reason, failure);
    }
    const auto reserved = reserve_batch(model, intent, session, now_us, durable_seq, plan);
    if (reserved.status != AYNI_OK) {
        return refuse(reserved.status, reserved.reason, failure);
    }
    return publish_batch(model, intent, session, plan, now_us, outcome) ? AYNI_OK : AYNI_INTERNAL;
}

} // namespace ayni::kernel
