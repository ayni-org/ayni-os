// Semantic checks and transitions of the intents, steps 4 and 5 of
// ayni_submit_intent in ayni.h (ADR-0017, ADR-0018). Every check runs before
// anything changes, and an accepted intent applies as one transition with
// one stamp, so a refused intent leaves no trace and allocates no id. The
// work tables check the object an intent names; work/propagation checks and
// applies what the change does to the tasks it reaches.

#include "kernel/service/command/intent_traits.h"
#include "kernel/service/intent/authority_intent.h"
#include "kernel/service/intent/context_intent.h"
#include "kernel/service/intent/memory_intent.h"
#include "kernel/service/memory/context_service.h"
#include "kernel/service/model/work_model.h"

namespace ayni::kernel {
namespace {

using work::GoalTable;

// Every intent return ends its canonical encoding borrow, including validation and capacity
// refusals after lookup. Only insert_exact may consume the candidate inside this scope.
struct CommandCandidate {
    CommandTable &table;
    ~CommandCandidate() { table.discard_candidate(); }
};

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
// wakes the owner so that it steps. Pause brings nothing forward, so it
// wakes no one (ADR-0018); its transition alone makes the next step search
// again and report the deadline without the goals it holds (ADR-0019).
void wake_for_due_work(IntentOutcome &outcome) {
    outcome.due_work = true;
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

// Resolve immutable catalog/work identities and validate parameters before
// the exporter runs. None of these checks publishes work or consumes IDs.
ayni_status_t check_goal_creation(WorkModel &model, const Intent &intent,
                                  const work::GoalOrigin &origin, uint64_t now_us,
                                  work::NewGoal &goal, IntentFailure &failure) {
    const GoalCreate &create = intent.create;
    const catalog::GoalTypeRegistry::Lookup type =
        model.catalogs.goal_types.find(create.goal_type_id, create.goal_type_version);
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
    if (!model.catalogs.goal_types.accepts_trigger(type.index, create.trigger.kind)) {
        return refuse(AYNI_INVALID_ARGUMENT, AYNI_REASON_TRIGGER_KIND,
                      object_ref(AYNI_OBJECT_GOAL_TYPE, 0), failure);
    }
    const work::AgentInstance *owner = model.agents.find_instance(create.owner_instance_id);
    if (owner == nullptr) {
        return refuse(AYNI_NOT_FOUND, AYNI_REASON_OWNER_UNKNOWN,
                      object_ref(AYNI_OBJECT_AGENT_INSTANCE, create.owner_instance_id), failure);
    }
    // A PAUSED owner still takes goals; their tasks wait until it resumes.
    if (!work::AgentTable::usable(owner)) {
        return refuse(
            AYNI_INVALID_STATE, AYNI_REASON_OWNER_STATE,
            object_ref(AYNI_OBJECT_AGENT_INSTANCE, create.owner_instance_id, owner->revision),
            failure);
    }
    catalog::CatalogId trigger_source;
    if (create.trigger.kind == AYNI_TRIGGER_LOCAL_INPUT) {
        static_cast<void>(trigger_source.assign(create.trigger_source)); // check_form()
    }
    const catalog::GoalType &goal_type = model.catalogs.goal_types.at(type.index);
    if (!goal_type.work_profile.accepts(create.trigger.kind, create.trigger.period_us)) {
        return refuse(AYNI_INVALID_ARGUMENT, AYNI_REASON_BACKGROUND_FREQUENCY,
                      object_ref(AYNI_OBJECT_GOAL_TYPE, 0), failure);
    }
    goal = work::NewGoal{create.owner_instance_id,
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
                         goal_type.version,
                         goal_type.work_profile};
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
    // Validate parameters before exporting their requirements. Both callbacks
    // are read-only under call_out; all admission finishes before IDs.
    const bool accepted = model.call_out(
        [&] { return model.catalogs.goal_types.accepts(type.index, create.parameters); });
    if (!accepted) {
        return refuse(AYNI_INVALID_ARGUMENT, AYNI_REASON_GOAL_PARAMETERS,
                      object_ref(AYNI_OBJECT_GOAL_TYPE, 0), failure);
    }

    return AYNI_OK;
}

// Resolve work and callback inputs before allocating an identity or publishing
// a slot. The remaining steps consume that unchanged, fully checked plan.
ayni_status_t create_goal(WorkModel &model, const Intent &intent, const work::GoalOrigin &origin,
                          uint64_t now_us, uint64_t durable_seq, IntentOutcome &outcome,
                          IntentFailure &failure, OwnerClock clock) {
    work::NewGoal goal{};
    const auto status = check_goal_creation(model, intent, origin, now_us, goal, failure);
    if (status != AYNI_OK) {
        return status;
    }
    const auto &goal_type = model.catalogs.goal_types.at(goal.goal_type_index);
    const auto &create = intent.create;
    work::GoalAuthorityRecord authority{};
    const auto requirements =
        model.prepare_goal_authority(goal_type, create, now_us, durable_seq, authority, clock);
    if (requirements.status != AYNI_OK) {
        return refuse(requirements.status, requirements.reason,
                      object_ref(AYNI_OBJECT_GOAL_TYPE, 0), failure);
    }
    GoalContent content{};
    const auto sources = prepare_goal_content({model, clock.now_us(), durable_seq}, goal_type,
                                              create.owner_instance_id, content);
    if (sources != AYNI_OK) {
        return refuse(sources, memory_reason(sources), {}, failure);
    }
    uint64_t goal_id = 0;
    static_cast<void>(model.ids.allocate(goal_id)); // available() above
    const foundation::TransitionStamp stamp = model.transitions.next();
    uint32_t index = 0;
    if (!model.insert_goal(goal, goal_id, stamp, index)) {
        // admit() found room and nothing could run since. The id and stamp
        // stay used up, as ayni.h says for this defect.
        return AYNI_INTERNAL;
    }
    if (work::is_calendar_trigger(create.trigger.kind)) {
        model.calendars.install(index, goal_id, create.calendar, stamp);
    }
    authority.goal_id = goal_id;
    if (content.origin.id != 0) {
        model.provenance.assign(work::ProvenanceKind::goal, index, goal_id, content.dependencies,
                                stamp, content.origin);
    }
    authority.stamp = stamp;
    if (authority.phase != work::GoalAuthorityPhase::proposed) {
        authority.binding.established_seq = stamp.commit_seq;
        const auto parent = model.goals.at(index).parent_index;
        if (parent != work::GoalTable::no_parent) {
            authority.binding.parent = {model.goals.at(parent).goal_id,
                                        model.goal_authority.at(parent).binding.established_seq};
        }
    }
    // stage() already checked configured capacity and normalized each operand.
    if (model.goal_authority.install_validated(index, authority) != AYNI_OK) {
        return AYNI_INTERNAL;
    }
    if (authority.phase == work::GoalAuthorityPhase::proposed) {
        model.goals.propose(index);
    }
    // Also when the occurrence or expiry is still to come: the host learns
    // its time from the next step's next_deadline_us.
    const work::Goal &created = model.goals.at(index);
    if (created.cursor.has_next() || created.expires_us != work::no_time) {
        wake_for_due_work(outcome);
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
    if (change != work::GoalChange::pause) {
        wake_for_due_work(outcome);
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
    wake_for_due_work(outcome);
    outcome.object = object_ref(AYNI_OBJECT_TASK, intent.target_id, model.tasks.at(index).revision);
    outcome.commit_seq = stamp.commit_seq;
    return AYNI_OK;
}

// Explicit owner preference uses the shared command window. Closing changes the
// in-memory gate immediately; enabling remains blocked until this stamp is durable.
ayni_status_t set_scene(WorkModel &model, const Intent &intent, IntentOutcome &outcome,
                        IntentFailure &failure) {
    authority::PreparedSceneChange prepared{};
    const auto status = model.scenes.prepare(intent.scene, prepared);
    if (status != AYNI_OK) {
        return refuse(status, AYNI_REASON_NONE, {}, failure);
    }
    uint64_t id = 0;
    if (prepared.state_index == authority::Scenes::no_index) {
        if (!model.ids.available()) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        static_cast<void>(model.ids.allocate(id));
    } else {
        id = model.scenes.state(prepared.state_index).object_id;
    }
    const auto stamp = model.transitions.next();
    model.scenes.apply(intent.scene, prepared, id, stamp);
    outcome.object = object_ref(AYNI_OBJECT_SCENE, id, prepared.next_revision);
    outcome.commit_seq = stamp.commit_seq;
    outcome.effect =
        prepared.changes_state ? AYNI_INTENT_EFFECT_CHANGED : AYNI_INTENT_EFFECT_NO_CHANGE;
    wake_for_due_work(outcome);
    return AYNI_OK;
}

// The Task owner validates its revision/ceiling; Kernel joins the exact owner
// command to that one transition. Raising removes only the runtime overlay.
ayni_status_t raise_task_limit(WorkModel &model, const Intent &intent, IntentOutcome &outcome,
                               IntentFailure &failure) {
    uint32_t index = 0;
    if (!model.tasks.find(intent.target_id, index)) {
        return refuse(AYNI_NOT_FOUND, AYNI_REASON_TASK_UNKNOWN,
                      object_ref(AYNI_OBJECT_TASK, intent.target_id), failure);
    }
    const auto current =
        object_ref(AYNI_OBJECT_TASK, intent.target_id, model.tasks.at(index).revision);
    const auto status = model.tasks.check_model_limit_raise(index, intent.expected_revision,
                                                            intent.model_call_limit);
    if (status != AYNI_OK) {
        const auto reason = status == AYNI_REVISION_CONFLICT   ? AYNI_REASON_TASK_REVISION
                            : status == AYNI_INVALID_STATE     ? AYNI_REASON_TASK_STATE
                            : status == AYNI_CAPACITY_EXCEEDED ? AYNI_REASON_REVISION_EXHAUSTED
                                                               : AYNI_REASON_NONE;
        return refuse(status, reason, current, failure);
    }
    const auto stamp = model.transitions.next();
    model.tasks.raise_model_limit(index, intent.model_call_limit, stamp);
    outcome.object = object_ref(AYNI_OBJECT_TASK, intent.target_id, model.tasks.at(index).revision);
    outcome.commit_seq = stamp.commit_seq;
    wake_for_due_work(outcome);
    return AYNI_OK;
}

// Resume changes only Task's wait overlay; dispatch independently rechecks authority.
ayni_status_t resume_task_wait(WorkModel &model, const Intent &intent, IntentOutcome &outcome,
                               IntentFailure &failure) {
    uint32_t index = 0;
    if (!model.tasks.find(intent.target_id, index)) {
        return refuse(AYNI_NOT_FOUND, AYNI_REASON_TASK_UNKNOWN,
                      object_ref(AYNI_OBJECT_TASK, intent.target_id), failure);
    }
    const auto current =
        object_ref(AYNI_OBJECT_TASK, intent.target_id, model.tasks.at(index).revision);
    const auto status = model.tasks.check_resume_wait(index, intent.expected_revision);
    if (status != AYNI_OK) {
        const auto reason = status == AYNI_REVISION_CONFLICT ? AYNI_REASON_TASK_REVISION
                            : status == AYNI_INVALID_STATE   ? AYNI_REASON_TASK_STATE
                                                             : AYNI_REASON_REVISION_EXHAUSTED;
        return refuse(status, reason, current, failure);
    }
    const auto stamp = model.transitions.next();
    model.tasks.resume_wait(index, stamp);
    outcome.object = object_ref(AYNI_OBJECT_TASK, intent.target_id, model.tasks.at(index).revision);
    outcome.commit_seq = stamp.commit_seq;
    wake_for_due_work(outcome);
    return AYNI_OK;
}

// Management authentication was checked once at the entry. Reconciliation
// only observes the original operation, so revoked execution Grants do not gate it.
ayni_status_t query_call(WorkModel &model, const Intent &intent, IntentOutcome &outcome,
                         IntentFailure &failure) {
    uint32_t index = 0;
    if (!model.calls.find(intent.target_id, index)) {
        return refuse(AYNI_NOT_FOUND, AYNI_REASON_NONE,
                      object_ref(AYNI_OBJECT_CALL, intent.target_id), failure);
    }
    const auto current =
        object_ref(AYNI_OBJECT_CALL, intent.target_id, model.calls.query_revision(index));
    const auto status = model.calls.check_query(index, intent.expected_revision);
    if (status != AYNI_OK) {
        return refuse(status, AYNI_REASON_NONE, current, failure);
    }
    const auto stamp = model.transitions.next();
    if (model.calls.request_query(index, {model.ids.boot_epoch(), stamp}) != AYNI_OK) {
        return AYNI_INTERNAL;
    }
    outcome.object =
        object_ref(AYNI_OBJECT_CALL, intent.target_id, model.calls.query_revision(index));
    outcome.commit_seq = stamp.commit_seq;
    wake_for_due_work(outcome);
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
    if (change != work::InstanceChange::pause) {
        wake_for_due_work(outcome);
    }
    outcome.object =
        object_ref(AYNI_OBJECT_AGENT_INSTANCE, intent.target_id, model.agents.at(index).revision);
    outcome.commit_seq = stamp.commit_seq;
    return AYNI_OK;
}

// Steps 4 and 5 once the session is known: the intent as such, without its
// command id.
ayni_status_t apply_rules(WorkModel &model, const Intent &intent, const authority::Session &session,
                          uint64_t now_us, uint64_t durable_seq, IntentOutcome &outcome,
                          IntentFailure &failure, OwnerClock clock, InputResumeSampler input) {
    if (is_memory_command(intent.kind)) {
        return apply_memory_intent(model, intent.memory, now_us, durable_seq, outcome);
    }
    if (is_context_command(intent.kind)) {
        return apply_context_intent(model, intent.content_context, now_us, durable_seq, outcome);
    }
    // Work and Grant management retain their separate invariant owners.
    switch (intent.kind) {
    case AYNI_INTENT_SCENE_SET_ENABLED:
        return set_scene(model, intent, outcome, failure);
    case AYNI_INTENT_GOAL_CREATE: {
        // The Host authenticated this exact binding when opening the
        // session; a goal keeps that origin independently of its lifetime.
        const work::GoalOrigin origin{AYNI_ORIGIN_USER, session.channel, session.principal_id,
                                      session.binding_revision};
        return create_goal(model, intent, origin, now_us, durable_seq, outcome, failure, clock);
    }
    case AYNI_INTENT_GRANT_CREATE:
        return create_grant(model, intent, session, now_us, durable_seq, outcome, failure);
    case AYNI_INTENT_GOAL_APPROVE:
        return approve_goal(model, intent, session, now_us, durable_seq, outcome, failure);
    case AYNI_INTENT_GOAL_REAUTHORIZE:
        return reauthorize_goal(model, intent, now_us, durable_seq, input, outcome, failure);
    case AYNI_INTENT_CALL_QUERY_RESULT:
        return query_call(model, intent, outcome, failure);
    case AYNI_INTENT_TASK_RESUME_WAIT:
        return resume_task_wait(model, intent, outcome, failure);
    case AYNI_INTENT_TASK_RAISE_MODEL_LIMIT:
        return raise_task_limit(model, intent, outcome, failure);
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

// The mapping joins the already-applied transition. Admission reserved its row
// and canonical bytes before domain changes, so a refusal here is a defect.
ayni_status_t remember_command(WorkModel &model, const Intent &intent, uint64_t principal_id,
                               uint64_t digest, const IntentOutcome &outcome) {
    CommandMapping mapping{principal_id,       intent.command_id, digest,
                           outcome.commit_seq, intent.kind,       outcome.object};
    mapping.effect = outcome.effect;
    uint32_t goal = 0;
    if (mapping.object.kind == AYNI_OBJECT_GOAL && model.goals.find(mapping.object.id, goal)) {
        const auto *sources =
            model.provenance.find(work::ProvenanceKind::goal, goal, mapping.object.id);
        if (sources != nullptr && sources->content == work::ContentState::available) {
            mapping.sources = sources->dependencies;
        }
    }
    model.release_command_sources(principal_id, outcome.commit_seq);
    switch (intent_traits(intent.kind).identity) {
    case CommandIdentity::ordinary:
        return model.commands.insert(mapping) ? AYNI_OK : AYNI_INTERNAL;
    case CommandIdentity::exact:
        return model.commands.insert_exact(mapping) ? AYNI_OK : AYNI_INTERNAL;
    case CommandIdentity::authorization: {
        if (!outcome.authorization) {
            return AYNI_INTERNAL;
        }
        const auto &result = *outcome.authorization;
        std::array<uint8_t, work_authority_result_size(AYNI_GOAL_REQUIREMENT_CAPACITY_MAX)> bytes{};
        write_work_authority_result(result, bytes.data());
        return model.commands.insert_exact(mapping,
                                           {bytes.data(), work_authority_result_size(result.count)})
                   ? AYNI_OK
                   : AYNI_INTERNAL;
    }
    case CommandIdentity::revoke:
        // Accepted into its pending control without a transition; the
        // mapping is published when the control applies.
        break;
    }
    return AYNI_INTERNAL;
}

// The storage checks of ayni.h step 4, and room in the retained command
// window, before applying any domain transition.
ayni_status_t check_room(const WorkModel &model, const Intent &intent, uint64_t principal_id,
                         bool has_room, ayni_status_t storage_status, IntentFailure &failure) {
    if (storage_status != AYNI_OK) {
        return refuse(AYNI_STORAGE_FAILED, AYNI_REASON_STORAGE_SPACE, ayni_object_ref_v1{},
                      failure);
    }
    if (!has_room) {
        return refuse(AYNI_BUSY, AYNI_REASON_STORAGE_BACKLOG, ayni_object_ref_v1{}, failure);
    }
    if (intent.command_id == 0U) {
        return AYNI_OK;
    }
    // Retained rows for principals no longer registered can fill the table.
    if (!model.commands.has_row(principal_id)) {
        return refuse(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_NONE,
                      object_ref(AYNI_OBJECT_PRINCIPAL, principal_id), failure);
    }
    if (!model.commands.can_insert(principal_id)) {
        return refuse(AYNI_BUSY, AYNI_REASON_COMMAND_PINNED, ayni_object_ref_v1{}, failure);
    }
    return AYNI_OK;
}

// Exact requests retain bytes independent of current catalog/target state.
// The common result also carries ordinary mappings and pending revoke receipts.
CommandTable::ExactLookup find_command(WorkModel &model, const Intent &intent,
                                       uint64_t principal_id, uint64_t digest) {
    if (intent_traits(intent.kind).identity == CommandIdentity::ordinary) {
        const CommandMapping *mapping = nullptr;
        const auto lookup = model.commands.find(principal_id, intent.command_id, digest, mapping);
        return {AYNI_OK, lookup, mapping};
    }
    if (is_memory_command(intent.kind)) {
        return model.commands.find_exact(principal_id, intent.command_id, intent.memory);
    }
    if (is_context_command(intent.kind)) {
        return model.commands.find_exact(principal_id, intent.command_id, intent.content_context);
    }
    // Each exact kind is looked up by its own canonical request.
    switch (intent.kind) {
    case AYNI_INTENT_SCENE_SET_ENABLED:
        return model.commands.find_exact(principal_id, intent.command_id, intent.scene);
    case AYNI_INTENT_CALL_QUERY_RESULT:
    case AYNI_INTENT_TASK_RESUME_WAIT:
        return model.commands.find_exact(
            principal_id, intent.command_id,
            TargetCommand{intent.kind, intent.expected_revision, intent.target_id});
    case AYNI_INTENT_TASK_RAISE_MODEL_LIMIT:
        return model.commands.find_exact(
            principal_id, intent.command_id,
            TaskLimitCommand{intent.expected_revision, intent.target_id, intent.model_call_limit});
    case AYNI_INTENT_GRANT_CREATE:
        return model.commands.find_exact(
            principal_id, intent.command_id,
            CanonicalCommandView{intent.expected_revision, &intent.grant_create});
    case AYNI_INTENT_GRANT_REVOKE: {
        const ayni_grant_revoke_v1 revoke{sizeof(revoke), 0, intent.target_id};
        return model.commands.find_revoke(principal_id, intent.command_id,
                                          CanonicalRevokeView{intent.expected_revision, &revoke});
    }
    case AYNI_INTENT_GOAL_APPROVE:
        return model.commands.find_exact(
            principal_id, intent.command_id,
            ApprovalCommand{intent.expected_revision, intent.approval});
    case AYNI_INTENT_GOAL_REAUTHORIZE:
        return model.commands.find_exact(
            principal_id, intent.command_id,
            ReauthorizationCommand{intent.expected_revision, intent.reauthorization});
    default: // an exact kind without a canonical request is a defect
        return {AYNI_INTERNAL, CommandTable::Lookup::unmapped, nullptr};
    }
}

} // namespace

ayni_status_t apply_intent(WorkModel &model, const Intent &intent, const IntentContext &context,
                           IntentOutcome &outcome, IntentFailure &failure) {
    const CommandCandidate candidate{model.commands};
    failure = IntentFailure{};
    const authority::Session *session = model.sessions.find(intent.session_id);
    if (session == nullptr) {
        return refuse(AYNI_DENIED, AYNI_REASON_SESSION_UNKNOWN,
                      object_ref(AYNI_OBJECT_SESSION, intent.session_id), failure);
    }
    const IntentTraits &traits = intent_traits(intent.kind);
    if (traits.session == SessionRule::management_on_retry &&
        !bound_management_session(model, *session, context.durable_seq)) {
        return refuse(AYNI_DENIED, AYNI_REASON_GRANT_CHANNEL, {}, failure);
    }
    const uint64_t digest = intent.command_id != 0U && traits.identity == CommandIdentity::ordinary
                                ? ordinary_command_digest(intent)
                                : 0U;
    if (intent.command_id != 0U) {
        const auto match = find_command(model, intent, session->principal_id, digest);
        if (match.status != AYNI_OK) {
            return match.status;
        }
        const auto *mapping = match.mapping;
        const auto *pending = match.pending;
        switch (match.lookup) {
        case CommandTable::Lookup::retried:
            if (pending != nullptr) {
                pending_revoke_outcome(*pending, mapping, outcome);
            } else {
                outcome.object = mapping->object;
                outcome.commit_seq = mapping->commit_seq;
                outcome.effect = mapping->effect;
            }
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
    // A revocation reserves its pending control instead, independent of
    // storage (ADR-0031).
    if (traits.identity != CommandIdentity::revoke) {
        const auto room = check_room(model, intent, session->principal_id, context.has_room,
                                     context.storage_status, failure);
        if (room != AYNI_OK) {
            return room;
        }
    }
    // The operation's own rules start here, so a retry returned above
    // without this check.
    if (traits.session == SessionRule::management &&
        !bound_management_session(model, *session, context.durable_seq)) {
        return refuse(AYNI_DENIED, AYNI_REASON_GRANT_CHANNEL, {}, failure);
    }
    if (traits.identity == CommandIdentity::revoke) {
        return accept_revoke(model, intent, *session, outcome, failure);
    }
    const ayni_status_t status =
        apply_rules(model, intent, *session, context.now_us, context.durable_seq, outcome, failure,
                    context.clock, context.input);
    return status == AYNI_OK && intent.command_id != 0U
               ? remember_command(model, intent, session->principal_id, digest, outcome)
               : status;
}

} // namespace ayni::kernel
