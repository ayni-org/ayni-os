// Semantic checks and transitions of the goal intents, steps 4 and 5 of
// ayni_submit_intent in ayni.h (ADR-0017). Every check runs before anything
// changes, and an accepted intent applies as one transition with one stamp,
// so a refused intent leaves no trace and allocates no id.

#include "kernel/intent/work_model.h"

namespace ayni::kernel {
namespace {

using work::GoalTable;

ayni_object_ref_v1 object_ref(ayni_object_kind_t kind, uint64_t id, uint32_t revision = 0) {
    return {0, id, revision, kind};
}

ayni_status_t refuse(ayni_status_t status, ayni_reason_t reason, ayni_object_ref_v1 object,
                     IntentFailure &failure) {
    failure.reason = reason;
    failure.object = object;
    return status;
}

work::GoalChange change_of(ayni_intent_kind_t kind) {
    switch (kind) {
    case AYNI_INTENT_GOAL_PAUSE:
        return work::GoalChange::pause;
    case AYNI_INTENT_GOAL_RESUME:
        return work::GoalChange::resume;
    default:
        return work::GoalChange::cancel;
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

ayni_status_t create_goal(WorkModel &model, const Intent &intent, const work::GoalOrigin &origin,
                          IntentOutcome &outcome, IntentFailure &failure) {
    const GoalCreate &create = intent.create;
    const catalog::GoalTypeRegistry::Lookup type =
        model.goal_types.find(create.goal_type_id, create.goal_type_version);
    switch (type.kind) {
    case catalog::GoalTypeRegistry::Lookup::Kind::unknown_id:
        return refuse(AYNI_NOT_FOUND, AYNI_REASON_GOAL_TYPE_UNKNOWN,
                      object_ref(AYNI_OBJECT_GOAL_TYPE, 0), failure);
    case catalog::GoalTypeRegistry::Lookup::Kind::unknown_version:
        return refuse(AYNI_UNSUPPORTED_VERSION, AYNI_REASON_GOAL_TYPE_VERSION,
                      object_ref(AYNI_OBJECT_GOAL_TYPE, 0), failure);
    case catalog::GoalTypeRegistry::Lookup::Kind::found:
        break;
    }
    if (model.agents.find_instance(create.owner_instance_id) == nullptr) {
        return refuse(AYNI_NOT_FOUND, AYNI_REASON_OWNER_UNKNOWN,
                      object_ref(AYNI_OBJECT_AGENT_INSTANCE, create.owner_instance_id), failure);
    }
    const work::NewGoal goal{create.owner_instance_id, create.parent_goal_id, type.index, origin,
                             create.parameters};
    const GoalTable::Admission admission = model.goals.admit(goal);
    if (admission != GoalTable::Admission::admitted) {
        return refuse_admission(admission, intent, failure);
    }
    if (!model.ids.available()) {
        return refuse(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_ID_EXHAUSTED, ayni_object_ref_v1{},
                      failure);
    }
    if (!model.goals.parameters_fit(create.parameters.size)) {
        return refuse(AYNI_INVALID_ARGUMENT, AYNI_REASON_GOAL_PARAMETERS,
                      object_ref(AYNI_OBJECT_GOAL_TYPE, 0), failure);
    }
    // The goal type's validator runs last, once nothing else can refuse the
    // goal, and cannot change the kernel while it runs (calling_out).
    model.calling_out = true;
    const bool accepted = model.goal_types.accepts(type.index, create.parameters);
    model.calling_out = false;
    if (!accepted) {
        return refuse(AYNI_INVALID_ARGUMENT, AYNI_REASON_GOAL_PARAMETERS,
                      object_ref(AYNI_OBJECT_GOAL_TYPE, 0), failure);
    }

    uint64_t goal_id = 0;
    static_cast<void>(model.ids.allocate(goal_id)); // available() above
    const foundation::TransitionStamp stamp = model.transitions.next();
    uint32_t index = 0;
    if (!model.goals.insert(goal, goal_id, stamp, index)) {
        // admit() found room and nothing could run since. The id and stamp
        // stay used up, as ayni.h says for this defect.
        return AYNI_INTERNAL;
    }
    outcome.object = object_ref(AYNI_OBJECT_GOAL, goal_id, model.goals.at(index).revision);
    outcome.commit_seq = stamp.commit_seq;
    return AYNI_OK;
}

ayni_status_t change_goal(WorkModel &model, const Intent &intent, IntentOutcome &outcome,
                          IntentFailure &failure) {
    uint32_t index = 0;
    if (!model.goals.find(intent.goal_id, index)) {
        return refuse(AYNI_NOT_FOUND, AYNI_REASON_GOAL_UNKNOWN,
                      object_ref(AYNI_OBJECT_GOAL, intent.goal_id), failure);
    }
    const work::GoalChange change = change_of(intent.kind);
    const ayni_object_ref_v1 current =
        object_ref(AYNI_OBJECT_GOAL, intent.goal_id, model.goals.at(index).revision);
    switch (model.goals.check(index, change, intent.expected_revision)) {
    case GoalTable::ChangeCheck::revision_conflict:
        return refuse(AYNI_REVISION_CONFLICT, AYNI_REASON_GOAL_REVISION, current, failure);
    case GoalTable::ChangeCheck::invalid_state:
        return refuse(AYNI_INVALID_STATE, AYNI_REASON_GOAL_STATE, current, failure);
    case GoalTable::ChangeCheck::revision_exhausted:
        return refuse(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_REVISION_EXHAUSTED, current, failure);
    case GoalTable::ChangeCheck::allowed:
        break;
    }

    const foundation::TransitionStamp stamp = model.transitions.next();
    model.goals.apply(index, change, stamp);
    outcome.object = object_ref(AYNI_OBJECT_GOAL, intent.goal_id, model.goals.at(index).revision);
    outcome.commit_seq = stamp.commit_seq;
    return AYNI_OK;
}

} // namespace

ayni_status_t apply_intent(WorkModel &model, const Intent &intent, IntentOutcome &outcome,
                           IntentFailure &failure) {
    failure = IntentFailure{};
    const authority::Session *session = model.sessions.find(intent.session_id);
    if (session == nullptr) {
        return refuse(AYNI_DENIED, AYNI_REASON_SESSION_UNKNOWN,
                      object_ref(AYNI_OBJECT_SESSION, intent.session_id), failure);
    }
    // Until grants exist (MA-006), every session belongs to the owner, who
    // may submit every goal intent.
    if (intent.kind != AYNI_INTENT_GOAL_CREATE) {
        return change_goal(model, intent, outcome, failure);
    }
    // Principals are never removed, so the session's principal is
    // registered.
    const authority::Principal &principal = *model.principals.find(session->principal_id);
    const work::GoalOrigin origin{AYNI_ORIGIN_USER, session->channel, principal.principal_id,
                                  principal.binding_revision};
    return create_goal(model, intent, origin, outcome, failure);
}

} // namespace ayni::kernel
