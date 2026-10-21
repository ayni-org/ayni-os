// Semantic checks and transitions of the intents, steps 4 and 5 of
// ayni_submit_intent in ayni.h (ADR-0017, ADR-0018). Every check runs before
// anything changes, and an accepted intent applies as one transition with
// one stamp, so a refused intent leaves no trace and allocates no id. The
// work tables check the object an intent names; work/propagation checks and
// applies what the change does to the tasks it reaches.

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

// A change that may bring the goals' due work forward: ayni_submit_intent
// wakes the owner so that it steps.
void mark_due_work(WorkModel &model, IntentOutcome &outcome) {
    model.due_work_changed = true;
    outcome.due_work = true;
}

// Pause brings nothing forward, so it wakes no one (ADR-0018), but the goals
// it holds may include the one due earliest: the next step searches again
// and reports the deadline without them (ADR-0019).
void mark_due_work_held(WorkModel &model) {
    model.due_work_changed = true;
}

// One check result type per table, with the same four outcomes.
template <typename ChangeCheck>
ayni_status_t refuse_change(ChangeCheck check, ayni_reason_t revision_reason,
                            ayni_reason_t state_reason, ayni_object_ref_v1 current,
                            IntentFailure &failure) {
    switch (check) {
    case ChangeCheck::revision_conflict:
        return refuse(AYNI_REVISION_CONFLICT, revision_reason, current, failure);
    case ChangeCheck::invalid_state:
        return refuse(AYNI_INVALID_STATE, state_reason, current, failure);
    case ChangeCheck::revision_exhausted:
        return refuse(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_REVISION_EXHAUSTED, current, failure);
    case ChangeCheck::allowed:
        break;
    }
    return AYNI_OK;
}

ayni_status_t refuse_exhausted(ayni_object_ref_v1 current, IntentFailure &failure) {
    return refuse(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_REVISION_EXHAUSTED, current, failure);
}

work::GoalChange goal_change_of(ayni_intent_kind_t kind) {
    switch (kind) {
    case AYNI_INTENT_GOAL_PAUSE:
        return work::GoalChange::pause;
    case AYNI_INTENT_GOAL_RESUME:
        return work::GoalChange::resume;
    default:
        return work::GoalChange::cancel;
    }
}

work::InstanceChange instance_change_of(ayni_intent_kind_t kind) {
    switch (kind) {
    case AYNI_INTENT_INSTANCE_PAUSE:
        return work::InstanceChange::pause;
    case AYNI_INTENT_INSTANCE_RESUME:
        return work::InstanceChange::resume;
    default:
        return work::InstanceChange::stop;
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
                          uint64_t now_us, IntentOutcome &outcome, IntentFailure &failure) {
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
    if (!model.goal_types.accepts_trigger(type.index, create.trigger.kind)) {
        return refuse(AYNI_INVALID_ARGUMENT, AYNI_REASON_TRIGGER_KIND,
                      object_ref(AYNI_OBJECT_GOAL_TYPE, 0), failure);
    }
    const work::AgentInstance *owner = model.agents.find_instance(create.owner_instance_id);
    if (owner == nullptr) {
        return refuse(AYNI_NOT_FOUND, AYNI_REASON_OWNER_UNKNOWN,
                      object_ref(AYNI_OBJECT_AGENT_INSTANCE, create.owner_instance_id), failure);
    }
    // A PAUSED owner still takes goals; their tasks wait until it resumes.
    const ayni_agent_lifecycle_t owner_lifecycle = work::AgentTable::effective_lifecycle(*owner);
    if (owner_lifecycle == AYNI_AGENT_STOPPED || owner_lifecycle == AYNI_AGENT_FAULTED) {
        return refuse(
            AYNI_INVALID_STATE, AYNI_REASON_OWNER_STATE,
            object_ref(AYNI_OBJECT_AGENT_INSTANCE, create.owner_instance_id, owner->revision),
            failure);
    }
    catalog::CatalogId trigger_source;
    if (create.trigger.kind == AYNI_TRIGGER_LOCAL_INPUT) {
        static_cast<void>(trigger_source.assign(create.trigger_source)); // check_form()
    }
    const catalog::GoalType &goal_type = model.goal_types.at(type.index);
    const work::NewGoal goal{create.owner_instance_id,
                             create.parent_goal_id,
                             type.index,
                             origin,
                             create.parameters,
                             create.trigger,
                             trigger_source,
                             now_us,
                             create.expiry_us,
                             goal_type.occurrence_wait_max_us,
                             goal_type.id,
                             goal_type.version};
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
    work::WorkTables tables = model.tables();
    if (!work::insert_goal(tables, goal, goal_id, stamp, index)) {
        // admit() found room and nothing could run since. The id and stamp
        // stay used up, as ayni.h says for this defect.
        return AYNI_INTERNAL;
    }
    // Also when the occurrence or expiry is still to come: the host learns
    // its time from the next step's next_deadline_us.
    const work::Goal &created = model.goals.at(index);
    if (created.cursor.has_next() || created.expires_us != work::no_time) {
        mark_due_work(model, outcome);
    }
    outcome.object = object_ref(AYNI_OBJECT_GOAL, goal_id, model.goals.at(index).revision);
    outcome.commit_seq = stamp.commit_seq;
    return AYNI_OK;
}

ayni_status_t change_goal(WorkModel &model, const Intent &intent, IntentOutcome &outcome,
                          IntentFailure &failure) {
    uint32_t index = 0;
    if (!model.goals.find(intent.target_id, index)) {
        return refuse(AYNI_NOT_FOUND, AYNI_REASON_GOAL_UNKNOWN,
                      object_ref(AYNI_OBJECT_GOAL, intent.target_id), failure);
    }
    const work::GoalChange change = goal_change_of(intent.kind);
    const ayni_object_ref_v1 current =
        object_ref(AYNI_OBJECT_GOAL, intent.target_id, model.goals.at(index).revision);
    const ayni_status_t status =
        refuse_change(model.goals.check(index, change, intent.expected_revision),
                      AYNI_REASON_GOAL_REVISION, AYNI_REASON_GOAL_STATE, current, failure);
    if (status != AYNI_OK) {
        return status;
    }
    work::WorkTables tables = model.tables();
    if (work::goal_change_exhausts_tasks(tables, index, change)) {
        return refuse_exhausted(current, failure);
    }

    const foundation::TransitionStamp stamp = model.transitions.next();
    work::apply_goal_change(tables, index, change, stamp);
    // Resume may let the goal's subtree advance; cancel ends tasks, whose
    // places other goals may be waiting for.
    if (change == work::GoalChange::pause) {
        mark_due_work_held(model);
    } else {
        mark_due_work(model, outcome);
    }
    outcome.object = object_ref(AYNI_OBJECT_GOAL, intent.target_id, model.goals.at(index).revision);
    outcome.commit_seq = stamp.commit_seq;
    return AYNI_OK;
}

ayni_status_t cancel_task(WorkModel &model, const Intent &intent, IntentOutcome &outcome,
                          IntentFailure &failure) {
    uint32_t index = 0;
    if (!model.tasks.find(intent.target_id, index)) {
        return refuse(AYNI_NOT_FOUND, AYNI_REASON_TASK_UNKNOWN,
                      object_ref(AYNI_OBJECT_TASK, intent.target_id), failure);
    }
    const ayni_object_ref_v1 current =
        object_ref(AYNI_OBJECT_TASK, intent.target_id, model.tasks.at(index).revision);
    const ayni_status_t status =
        refuse_change(model.tasks.check(index, work::TaskChange::cancel, intent.expected_revision),
                      AYNI_REASON_TASK_REVISION, AYNI_REASON_TASK_STATE, current, failure);
    if (status != AYNI_OK) {
        return status;
    }
    // The goal's last task takes the goal with it (ADR-0023).
    work::WorkTables tables = model.tables();
    if (work::task_end_exhausts(tables, index, work::TaskChange::cancel)) {
        return refuse_exhausted(current, failure);
    }

    const foundation::TransitionStamp stamp = model.transitions.next();
    work::end_task(tables, index, work::TaskChange::cancel, stamp);
    // The ended task's place may be what another goal is waiting for.
    mark_due_work(model, outcome);
    outcome.object = object_ref(AYNI_OBJECT_TASK, intent.target_id, model.tasks.at(index).revision);
    outcome.commit_seq = stamp.commit_seq;
    return AYNI_OK;
}

ayni_status_t change_instance(WorkModel &model, const Intent &intent, IntentOutcome &outcome,
                              IntentFailure &failure) {
    uint32_t index = 0;
    if (!model.agents.find_instance(intent.target_id, index)) {
        return refuse(AYNI_NOT_FOUND, AYNI_REASON_INSTANCE_UNKNOWN,
                      object_ref(AYNI_OBJECT_AGENT_INSTANCE, intent.target_id), failure);
    }
    const work::InstanceChange change = instance_change_of(intent.kind);
    const ayni_object_ref_v1 current =
        object_ref(AYNI_OBJECT_AGENT_INSTANCE, intent.target_id, model.agents.at(index).revision);
    const ayni_status_t status =
        refuse_change(model.agents.check(index, change, intent.expected_revision),
                      AYNI_REASON_INSTANCE_REVISION, AYNI_REASON_INSTANCE_STATE, current, failure);
    if (status != AYNI_OK) {
        return status;
    }
    work::WorkTables tables = model.tables();
    if (work::instance_change_exhausts(tables, index, change)) {
        return refuse_exhausted(current, failure);
    }

    const foundation::TransitionStamp stamp = model.transitions.next();
    work::apply_instance_change(tables, index, change, stamp);
    // Resume may let the instance's goals advance; stop ends tasks, whose
    // places other instances' goals may be waiting for.
    if (change == work::InstanceChange::pause) {
        mark_due_work_held(model);
    } else {
        mark_due_work(model, outcome);
    }
    outcome.object =
        object_ref(AYNI_OBJECT_AGENT_INSTANCE, intent.target_id, model.agents.at(index).revision);
    outcome.commit_seq = stamp.commit_seq;
    return AYNI_OK;
}

// Steps 4 and 5 once the session is known: the intent as such, without its
// command id.
ayni_status_t apply_rules(WorkModel &model, const Intent &intent, const authority::Session &session,
                          uint64_t now_us, IntentOutcome &outcome, IntentFailure &failure) {
    // Until grants exist (MA-006), every session belongs to the owner, who
    // may submit every intent.
    switch (intent.kind) {
    case AYNI_INTENT_GOAL_CREATE: {
        // Principals are never removed, so the session's principal is
        // registered.
        const authority::Principal &principal = *model.principals.find(session.principal_id);
        const work::GoalOrigin origin{AYNI_ORIGIN_USER, session.channel, principal.principal_id,
                                      principal.binding_revision};
        return create_goal(model, intent, origin, now_us, outcome, failure);
    }
    case AYNI_INTENT_TASK_CANCEL:
        return cancel_task(model, intent, outcome, failure);
    case AYNI_INTENT_INSTANCE_PAUSE:
    case AYNI_INTENT_INSTANCE_RESUME:
    case AYNI_INTENT_INSTANCE_STOP:
        return change_instance(model, intent, outcome, failure);
    default: // GOAL_CANCEL, GOAL_PAUSE, GOAL_RESUME
        return change_goal(model, intent, outcome, failure);
    }
}

} // namespace

ayni_status_t apply_intent(WorkModel &model, const Intent &intent, uint64_t now_us, bool has_room,
                           IntentOutcome &outcome, IntentFailure &failure,
                           ayni_status_t storage_status) {
    failure = IntentFailure{};
    const authority::Session *session = model.sessions.find(intent.session_id);
    if (session == nullptr) {
        return refuse(AYNI_DENIED, AYNI_REASON_SESSION_UNKNOWN,
                      object_ref(AYNI_OBJECT_SESSION, intent.session_id), failure);
    }
    const uint64_t digest = intent.command_id != 0U ? command_digest(intent) : 0U;
    if (intent.command_id != 0U) {
        const CommandMapping *mapping = nullptr;
        switch (model.commands.find(session->principal_id, intent.command_id, digest, mapping)) {
        case CommandTable::Lookup::retried:
            outcome.object = mapping->object;
            outcome.commit_seq = mapping->commit_seq;
            outcome.retried = true;
            return AYNI_OK;
        case CommandTable::Lookup::conflict:
            return refuse(AYNI_IDEMPOTENCY_CONFLICT, AYNI_REASON_COMMAND_CONFLICT,
                          ayni_object_ref_v1{}, failure);
        case CommandTable::Lookup::expired:
            return refuse(AYNI_EXPIRED, AYNI_REASON_COMMAND_EXPIRED, ayni_object_ref_v1{}, failure);
        case CommandTable::Lookup::unmapped:
            break;
        }
    }
    if (storage_status != AYNI_OK) {
        return refuse(AYNI_STORAGE_FAILED, AYNI_REASON_STORAGE_SPACE, ayni_object_ref_v1{},
                      failure);
    }
    if (!has_room) {
        return refuse(AYNI_BUSY, AYNI_REASON_STORAGE_BACKLOG, ayni_object_ref_v1{}, failure);
    }
    if (intent.command_id != 0U && !model.commands.has_row(session->principal_id)) {
        return refuse(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_NONE,
                      object_ref(AYNI_OBJECT_PRINCIPAL, session->principal_id), failure);
    }
    const ayni_status_t status = apply_rules(model, intent, *session, now_us, outcome, failure);
    // The mapping joins the transition, so the log stores both or neither.
    // A row retained for an unregistered principal can occupy the table;
    // admission above reserves a row before any domain transition changes.
    if (status == AYNI_OK && intent.command_id != 0U) {
        model.commands.insert({session->principal_id, intent.command_id, digest, outcome.commit_seq,
                               intent.kind, outcome.object});
    }
    return status;
}

} // namespace ayni::kernel
