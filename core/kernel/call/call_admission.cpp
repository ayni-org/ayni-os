#include "kernel/call/call_runtime.h"

#include "authority/scope/scope.h"

#include <cstring>

namespace ayni::kernel {
namespace {
constexpr uint32_t identities_per_call =
    2; // Logical call and operation, both stable before dispatch.

ayni_string_v1 view(const catalog::CatalogId &id) {
    return {id.data(), id.size()};
}

// Canonical actual arguments are the only routing authority. Static membership
// in the registered domain was checked at start; execution uses these same bytes.
bool routing_matches(const catalog::Binding &binding, ayni_bytes_v1 arguments) {
    catalog::DomainReader reader(arguments);
    catalog::DomainField field;
    while (reader.next(field)) {
        if (field.usage == AYNI_AUTHORITY_EXECUTION_TARGET &&
            !binding.endpoint.equals(field.values.tokens[0])) {
            return false;
        }
        if (field.usage == AYNI_AUTHORITY_EGRESS_DESTINATION &&
            !binding.destination.equals(field.values.tokens[0])) {
            return false;
        }
    }
    return reader.ok();
}
} // namespace

void CallRuntime::begin_decision(uint32_t task_index, ayni_task_snapshot_v1 &snapshot,
                                 ayni_policy_output_v1 &output) {
    proposal_.reset();
    if (limits_.capacity == 0U) {
        return;
    }
    decision_ = Decision{task_index, output.intents, &output};
    output.call_context = this;
    output.request_call = stage_request;
    snapshot.call_context = this;
    snapshot.call_result = read_result;
}

void CallRuntime::end_decision() {
    decision_.reset();
}

ayni_status_t CallRuntime::stage_request(void *context, const ayni_call_request_v1 *request) {
    if (context == nullptr || request == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &runtime = *static_cast<CallRuntime *>(context);
    return runtime.decision_ && runtime.work_.calling_out() ? runtime.stage(*request)
                                                            : AYNI_INVALID_STATE;
}

// Copy while every nested argument view is still live. A proposal is not a
// domain mutation and does not consume a step, call count, resource or identity.
ayni_status_t CallRuntime::stage(const ayni_call_request_v1 &request) {
    if (request.struct_size != sizeof(request) || request.reserved != 0U ||
        (request.kind != AYNI_POLICY_REQUEST_CALL && request.kind != AYNI_POLICY_REQUEST_MODEL)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (proposal_) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    Proposal proposal{};
    if (!proposal.step.assign(request.step_key) ||
        !catalogs_.bindings.find(request.binding_id, request.binding_version,
                                 proposal.binding_index)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &binding = catalogs_.bindings.at(proposal.binding_index);
    const auto resolved =
        catalog::BindingRegistry::resolve(binding, work_.capabilities, catalogs_.capabilities,
                                          catalogs_.providers, catalogs_.resources, proposal.links);
    if (resolved != AYNI_OK) {
        return resolved;
    }
    const auto &contract = catalogs_.capabilities.at(proposal.links.call).contract;
    if (contract.form == catalog::CallForm::subscription) {
        return AYNI_UNSUPPORTED; // Subscription lifecycle is delivered separately (ADR-0050).
    }
    const bool physical = (contract.effects & catalog::call_effect_physical) != 0U;
    uint32_t execution = 0;
    if (physical && !work_.execution_catalog.find_binding(request.binding_id,
                                                          request.binding_version, execution)) {
        return AYNI_UNSUPPORTED;
    }
    for (uint32_t at = 0; at < contract.resource_count; ++at) {
        if (!physical && (catalogs_.resources.at(proposal.links.resources[at]).flags &
                          AYNI_RESOURCE_ACTUATOR) != 0U) {
            return AYNI_UNSUPPORTED; // Every actuator acquisition uses the same permit/stop
                                     // consumer.
        }
    }
    if ((contract.category == catalog::CallClass::model) !=
        (request.kind == AYNI_POLICY_REQUEST_MODEL)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto status =
        work::encode_call_input(work_.capabilities.domain_view(proposal.links.capability),
                                {request.arguments, request.text}, contract.input_text_limit,
                                staging_, limits_.input_bytes, proposal.input_size);
    if (status != AYNI_OK) {
        return status;
    }
    proposal.task_id = work_.tasks.at(decision_->task_index).task_id;
    proposal.kind = request.kind;
    proposal_ = proposal;
    auto &intent = decision_->intents[0];
    intent = {};
    intent.struct_size = sizeof(intent);
    intent.kind = request.kind;
    intent.expected_revision = work_.tasks.at(decision_->task_index).revision;
    intent.step_key_size = proposal.step.size();
    std::memcpy(intent.step_key, proposal.step.data(), proposal.step.size());
    decision_->output->intent_count = 1;
    return AYNI_OK;
}

// This accessor is Task-scoped and usable only during decide. It exposes no
// Provider context or another Agent's bodies, and never claims volatile output durable.
ayni_status_t CallRuntime::read_result(const void *context, ayni_string_v1 step,
                                       ayni_task_call_result_v1 *out) {
    if (context == nullptr || out == nullptr || out->struct_size != sizeof(*out)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &runtime = *static_cast<const CallRuntime *>(context);
    if (!runtime.decision_ || !runtime.work_.calling_out()) {
        return AYNI_INVALID_STATE;
    }
    const auto &task = runtime.work_.tasks.at(runtime.decision_->task_index);
    uint32_t index = 0;
    if (!runtime.work_.calls.find_step(task.task_id, step, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto &calls = runtime.work_.calls;
    const auto &state = calls.state(index);
    *out = {sizeof(*out),
            state.outcome,
            calls.identity(index).call_id,
            calls.identity(index).operation_id,
            state.outcome == 0U ? 0U : state.latest.transition.commit_seq,
            state.status,
            state.reason,
            calls.result(index)};
    return AYNI_OK;
}

// Reuse Task/Goal's selected complete Grant, then prove these actual values are
// within one immutable requirement. No union of grants or authority-only shadow input.
authority::GrantCheck CallRuntime::check_input(uint32_t task_index, const catalog::Binding &binding,
                                               ayni_bytes_v1 domain,
                                               const work::CallInputView &input, uint64_t now_us,
                                               uint64_t durable_seq) const {
    const auto authority = work_.check_task_authority(task_index, now_us, durable_seq);
    if (authority.status != AYNI_OK) {
        return authority;
    }
    if (!routing_matches(binding, input.arguments)) {
        return {AYNI_DENIED, AYNI_REASON_GRANT_SCOPE};
    }
    const auto &task = work_.tasks.at(task_index);
    const auto ceiling = work_.goal_authority.at(task.goal_index);
    for (uint32_t at = 0; at < ceiling.count; ++at) {
        const auto &requirement = ceiling.requirements[at];
        if (!requirement.capability_id.equals(view(binding.capability)) ||
            requirement.capability_version != binding.capability_version ||
            requirement.source_domain.size != domain.size ||
            std::memcmp(requirement.source_domain.data, domain.data, domain.size) != 0) {
            continue;
        }
        const auto coverage =
            authority::contains_request(domain, requirement.scope, input.arguments);
        if (coverage.status == AYNI_OK && coverage.covered) {
            return {AYNI_OK, AYNI_REASON_NONE};
        }
    }
    return {AYNI_DENIED, AYNI_REASON_GRANT_SCOPE};
}

// Ordinary resource groups use the same round-robin queue even with zero
// resources. Sort resolved indices, keeping units attached to their resource.
resources::ResourceRequest
CallRuntime::resource_request(uint64_t call_id, uint64_t owner, const catalog::BindingLinks &links,
                              const catalog::CallContract &contract,
                              const catalog::WorkProfile &profile) const {
    resources::ResourceRequest request{call_id,
                                       owner,
                                       resources::Priority::normal,
                                       resources::WaitAction::pause,
                                       AYNI_RESOURCE_WAIT_DEFAULT_US,
                                       contract.resource_count,
                                       {}};
    switch (profile.kind) {
    case AYNI_WORK_FOREGROUND:
        request.priority = resources::Priority::foreground;
        break;
    case AYNI_WORK_REMINDER:
        request.priority = resources::Priority::foreground;
        request.on_wait = resources::WaitAction::wait;
        request.wait_us = resources::no_deadline;
        break;
    case AYNI_WORK_BACKGROUND:
        request.priority = resources::Priority::background;
        request.on_wait = resources::WaitAction::skip;
        break;
    default:
        break; // ORDINARY preserves the existing normal queue and explicit pause semantics.
    }
    for (uint32_t at = 0; at < contract.resource_count; ++at) {
        const resources::ResourceUnits units{links.resources[at], contract.resources[at].units};
        uint32_t place = at;
        while (place != 0U && request.resources[place - 1U].index > units.index) {
            request.resources[place] = request.resources[place - 1U];
            --place;
        }
        request.resources[place] = units;
    }
    return request;
}

// All fallible checks precede the decision's transition. Existing result bodies
// cannot feed another external call until the preceding Task/result batch is durable.
authority::GrantCheck CallRuntime::check_proposal(uint32_t task_index,
                                                  const ayni_policy_intent_v1 &intent,
                                                  uint64_t now_us, uint64_t durable_seq) {
    const auto &task = work_.tasks.at(task_index);
    if (!proposal_ || proposal_->task_id != task.task_id || proposal_->kind != intent.kind ||
        !proposal_->step.equals({intent.step_key, intent.step_key_size})) {
        return {AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_INTENT};
    }
    const auto &proposal = *proposal_;
    const auto &contract = catalogs_.capabilities.at(proposal.links.call).contract;
    if (task.call.id != 0U || work_.step_taken(task_index, view(proposal.step))) {
        return {AYNI_INVALID_ARGUMENT, AYNI_REASON_STEP_TAKEN};
    }
    if (!work::TaskTable::can_begin_call(task)) {
        return {AYNI_CAPACITY_EXCEEDED, AYNI_REASON_REVISION_EXHAUSTED};
    }
    if (contract.category == catalog::CallClass::model &&
        task.model_calls.used >= task.model_calls.limit) {
        return {AYNI_CAPACITY_EXCEEDED, AYNI_REASON_TASK_MODEL_CALL_LIMIT};
    }
    if (work_.accepted_step_count(task_index) >= work_.tasks.step_capacity()) {
        return {AYNI_CAPACITY_EXCEEDED, AYNI_REASON_STEP_CAPACITY};
    }
    if (!work_.calls.has_room() || !resources_.has_room() ||
        !work_.ids.available(identities_per_call) ||
        contract.result_text_limit > limits_.result_bytes) {
        return {AYNI_CAPACITY_EXCEEDED, AYNI_REASON_POLICY_OUTPUT_LIMIT};
    }
    const auto domain = work_.capabilities.domain_view(proposal.links.capability);
    work::CallInputView input{};
    if (work::read_call_input(domain, {staging_, proposal.input_size}, contract.input_text_limit,
                              input) != AYNI_OK) {
        return {AYNI_INTERNAL, AYNI_REASON_NONE};
    }
    const auto authority = check_input(task_index, catalogs_.bindings.at(proposal.binding_index),
                                       domain, input, now_us, durable_seq);
    return authority.status == AYNI_OK
               ? prepare_execution(*proposal_, input.arguments, now_us, durable_seq)
               : authority;
}

// Exact capacity and identity preflights have succeeded. Any refusal here is an
// internal invariant failure; no callback can intervene between check and apply.
bool CallRuntime::accept(uint32_t task_index, foundation::TransitionStamp stamp, uint64_t now_us) {
    const auto &proposal = *proposal_;
    const auto &binding = catalogs_.bindings.at(proposal.binding_index);
    const auto &contract = catalogs_.capabilities.at(proposal.links.call).contract;
    const auto &task = work_.tasks.at(task_index);
    work::CallIdentity identity{};
    if (!work_.ids.allocate(identity.call_id) || !work_.ids.allocate(identity.operation_id)) {
        return false;
    }
    identity.task_id = task.task_id;
    identity.owner_instance_id = task.owner_instance_id;
    identity.step_key = proposal.step;
    identity.binding_id = binding.id;
    identity.binding_version = binding.version;
    identity.capability_id = binding.capability;
    identity.capability_version = binding.capability_version;
    identity.provider_id = binding.provider;
    identity.provider_version = binding.provider_version;
    identity.endpoint = binding.endpoint;
    identity.destination = binding.destination;
    uint32_t index = 0;
    if (work_.calls.insert(
            identity, contract, work_.capabilities.domain_view(proposal.links.capability),
            {staging_, proposal.input_size}, {work_.ids.boot_epoch(), stamp}, index) != AYNI_OK ||
        resources_.enqueue(resource_request(identity.call_id, task.owner_instance_id,
                                            proposal.links, contract,
                                            work_.goals.at(task.goal_index).work_profile),
                           now_us) != AYNI_OK) {
        return false;
    }
    if (proposal.execution) {
        work_.calls.attach_execution(index, *proposal.execution);
    }
    slots_[index].active = Active{identity.call_id, Stage::queued, {}, std::nullopt};
    work_.tasks.begin_call(task_index, identity.call_id,
                           contract.category == catalog::CallClass::model
                               ? work::TaskTable::CallKind::model
                               : work::TaskTable::CallKind::tool,
                           stamp);
    proposal_.reset();
    return true;
}
} // namespace ayni::kernel
