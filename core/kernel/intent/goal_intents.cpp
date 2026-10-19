// Semantic checks and transitions of the goal intents, steps 3 to 5 of
// ayni_submit_intent in ayni.h (ADR-0017). Every check runs before anything
// changes, and an accepted intent applies as one transition with one stamp,
// so a refused intent leaves no trace and allocates no id.

#include "kernel/kernel_state.h"

namespace {

using ayni::kernel::Intent;
using ayni::kernel::IntentFailure;
using ayni::kernel::IntentOutcome;
using ayni::work::GoalTable;

ayni_object_ref_v1 object_ref(ayni_object_kind_t kind, uint64_t id, uint32_t revision = 0) {
    return {0, id, revision, kind};
}

ayni_status_t refuse(ayni_status_t status, ayni_reason_t reason, ayni_object_ref_v1 object,
                     IntentFailure &failure) {
    failure.reason = reason;
    failure.object = object;
    return status;
}

ayni::work::GoalChange change_of(ayni_intent_kind_t kind) {
    switch (kind) {
    case AYNI_INTENT_GOAL_PAUSE:
        return ayni::work::GoalChange::pause;
    case AYNI_INTENT_GOAL_RESUME:
        return ayni::work::GoalChange::resume;
    default:
        return ayni::work::GoalChange::cancel;
    }
}

ayni_status_t refuse_admission(GoalTable::Admission admission, const Intent &intent,
                               IntentFailure &failure) {
    const ayni_object_ref_v1 parent = object_ref(AYNI_OBJECT_GOAL, intent.create.parent_goal_id);
    switch (admission) {
    case GoalTable::Admission::parent_unknown:
        return refuse(AYNI_NOT_FOUND, AYNI_REASON_PARENT_UNKNOWN, parent, failure);
    case GoalTable::Admission::parent_state:
        return refuse(AYNI_INVALID_STATE, AYNI_REASON_PARENT_STATE, parent, failure);
    case GoalTable::Admission::parent_owner:
        return refuse(AYNI_INVALID_ARGUMENT, AYNI_REASON_PARENT_OWNER, parent, failure);
    case GoalTable::Admission::too_deep:
        return refuse(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_GOAL_DEPTH, parent, failure);
    case GoalTable::Admission::too_many_children:
        return refuse(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_GOAL_CHILDREN, parent, failure);
    case GoalTable::Admission::full:
    case GoalTable::Admission::admitted:
        break;
    }
    return refuse(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_GOAL_CAPACITY, ayni_object_ref_v1{}, failure);
}

} // namespace

ayni_status_t ayni_kernel::submit_intent(const Intent &intent, IntentOutcome &outcome,
                                         IntentFailure &failure) {
    failure = IntentFailure{};
    if (lifecycle != AYNI_LIFECYCLE_RUNNING) {
        return AYNI_NOT_READY;
    }
    const ayni::authority::Session *session = sessions.find(intent.session_id);
    if (session == nullptr) {
        return refuse(AYNI_DENIED, AYNI_REASON_SESSION_UNKNOWN,
                      object_ref(AYNI_OBJECT_SESSION, intent.session_id), failure);
    }
    // Until grants exist (MA-006), every session belongs to the owner, who
    // may submit every goal intent.
    if (intent.kind != AYNI_INTENT_GOAL_CREATE) {
        return change_goal(intent, outcome, failure);
    }
    // Sessions close before their principal could go: principals are never
    // removed, so the session's principal is registered.
    const ayni::authority::Principal &principal = *principals.find(session->principal_id);
    const ayni::work::GoalOrigin origin{AYNI_ORIGIN_USER, session->channel, principal.principal_id,
                                        principal.binding_revision};
    return create_goal(intent, origin, outcome, failure);
}

ayni_status_t ayni_kernel::create_goal(const Intent &intent, const ayni::work::GoalOrigin &origin,
                                       IntentOutcome &outcome, IntentFailure &failure) {
    const ayni::kernel::GoalCreate &create = intent.create;
    const ayni::catalog::GoalTypeRegistry::Lookup type =
        goal_types.find(create.goal_type_id, create.goal_type_version);
    switch (type.kind) {
    case ayni::catalog::GoalTypeRegistry::Lookup::Kind::unknown_id:
        return refuse(AYNI_NOT_FOUND, AYNI_REASON_GOAL_TYPE_UNKNOWN,
                      object_ref(AYNI_OBJECT_GOAL_TYPE, 0), failure);
    case ayni::catalog::GoalTypeRegistry::Lookup::Kind::unknown_version:
        return refuse(AYNI_UNSUPPORTED_VERSION, AYNI_REASON_GOAL_TYPE_VERSION,
                      object_ref(AYNI_OBJECT_GOAL_TYPE, 0), failure);
    case ayni::catalog::GoalTypeRegistry::Lookup::Kind::found:
        break;
    }
    if (agents.find_instance(create.owner_instance_id) == nullptr) {
        return refuse(AYNI_NOT_FOUND, AYNI_REASON_OWNER_UNKNOWN,
                      object_ref(AYNI_OBJECT_AGENT_INSTANCE, create.owner_instance_id), failure);
    }
    const ayni::work::NewGoal goal{create.owner_instance_id, create.parent_goal_id, type.index,
                                   origin, create.parameters};
    const GoalTable::Admission admission = goals.admit(goal);
    if (admission != GoalTable::Admission::admitted) {
        return refuse_admission(admission, intent, failure);
    }
    if (!ids.available()) {
        return refuse(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_ID_EXHAUSTED, ayni_object_ref_v1{},
                      failure);
    }
    // The goal type's validator runs last, once nothing else can refuse the
    // goal, and before anything changes.
    if (!goals.parameters_fit(create.parameters.size) ||
        !goal_types.accepts(type.index, create.parameters)) {
        return refuse(AYNI_INVALID_ARGUMENT, AYNI_REASON_GOAL_PARAMETERS,
                      object_ref(AYNI_OBJECT_GOAL_TYPE, 0), failure);
    }

    uint64_t goal_id = 0;
    static_cast<void>(ids.allocate(goal_id)); // available() above
    const ayni::foundation::TransitionStamp stamp = transitions.next();
    const uint32_t index = goals.insert(goal, goal_id, stamp);
    outcome.object = object_ref(AYNI_OBJECT_GOAL, goal_id, goals.at(index).revision);
    outcome.commit_seq = stamp.commit_seq;
    return AYNI_OK;
}

ayni_status_t ayni_kernel::change_goal(const Intent &intent, IntentOutcome &outcome,
                                       IntentFailure &failure) {
    uint32_t index = 0;
    if (!goals.find(intent.goal_id, index)) {
        return refuse(AYNI_NOT_FOUND, AYNI_REASON_GOAL_UNKNOWN,
                      object_ref(AYNI_OBJECT_GOAL, intent.goal_id), failure);
    }
    const ayni::work::GoalChange change = change_of(intent.kind);
    const ayni_object_ref_v1 current =
        object_ref(AYNI_OBJECT_GOAL, intent.goal_id, goals.at(index).revision);
    switch (goals.check(index, change, intent.expected_revision)) {
    case GoalTable::ChangeCheck::revision_conflict:
        return refuse(AYNI_REVISION_CONFLICT, AYNI_REASON_GOAL_REVISION, current, failure);
    case GoalTable::ChangeCheck::invalid_state:
        return refuse(AYNI_INVALID_STATE, AYNI_REASON_GOAL_STATE, current, failure);
    case GoalTable::ChangeCheck::revision_exhausted:
        return refuse(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_REVISION_EXHAUSTED, current, failure);
    case GoalTable::ChangeCheck::allowed:
        break;
    }

    const ayni::foundation::TransitionStamp stamp = transitions.next();
    goals.apply(index, change, stamp);
    outcome.object = object_ref(AYNI_OBJECT_GOAL, intent.goal_id, goals.at(index).revision);
    outcome.commit_seq = stamp.commit_seq;
    return AYNI_OK;
}
