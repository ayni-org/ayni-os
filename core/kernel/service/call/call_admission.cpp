#include "kernel/service/call/call_runtime.h"
#include "kernel/service/memory/memory_permissions.h"

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
                                 ayni_policy_output_v1 &output, uint64_t durable_seq) {
    proposal_.reset();
    if (limits_.capacity == 0U) {
        return;
    }
    decision_ = Decision{task_index, durable_seq, output.intents, &output};
    output.call_context = this;
    output.request_call = stage_request;
    snapshot.call_context = this;
    snapshot.call_result = read_result;
    snapshot.call_tool = read_tool_call;
    snapshot.subscription = read_subscription;
    snapshot.subscription_observation = read_subscription_observation;
}

void CallRuntime::end_decision() {
    decision_.reset();
}

void CallRuntime::scrub_decision() {
    proposal_.reset();
    if (staging_ != nullptr) {
        std::memset(staging_, 0, limits_.input_bytes);
    }
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
        (request.kind != AYNI_POLICY_REQUEST_CALL && request.kind != AYNI_POLICY_REQUEST_MODEL &&
         request.kind != AYNI_POLICY_CLOSE_SUBSCRIPTION &&
         request.kind != AYNI_POLICY_WAIT_SUBSCRIPTION)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (proposal_) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    if (request.kind == AYNI_POLICY_CLOSE_SUBSCRIPTION ||
        request.kind == AYNI_POLICY_WAIT_SUBSCRIPTION) {
        return stage_subscription_target(request);
    }
    Proposal proposal{};
    if (!proposal.step.assign(request.step_key) ||
        !catalogs_.bindings.find(request.binding_id, request.binding_version,
                                 proposal.binding_index)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &binding = catalogs_.bindings.at(proposal.binding_index);
    const auto resolved = catalog::BindingRegistry::resolve(
        binding, catalogs_.capabilities, catalogs_.call_capabilities, catalogs_.providers,
        catalogs_.resources, proposal.links);
    if (resolved != AYNI_OK) {
        return resolved;
    }
    const auto dispatchable = catalogs_.dispatchable(binding, proposal.links);
    if (dispatchable != AYNI_OK) {
        return dispatchable;
    }
    const auto &contract = catalogs_.call_capabilities.at(proposal.links.call).contract;
    if ((contract.category == catalog::CallClass::model) !=
        (request.kind == AYNI_POLICY_REQUEST_MODEL)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto status =
        work::encode_call_input(catalogs_.capabilities.domain_view(proposal.links.capability),
                                {request.arguments, request.text}, contract.input_text_limit,
                                staging_, limits_.input_bytes, proposal.input_size);
    if (status != AYNI_OK) {
        return status;
    }
    proposal.task_id = work_.tasks.at(decision_->task_index).task_id;
    proposal.kind = request.kind;
    stage_intent(proposal);
    return AYNI_OK;
}
void CallRuntime::stage_intent(const Proposal &proposal) {
    proposal_ = proposal;
    auto &intent = decision_->intents[0];
    intent = {};
    intent.struct_size = sizeof(intent);
    intent.kind = proposal.kind;
    intent.expected_revision = work_.tasks.at(decision_->task_index).revision;
    intent.step_key_size = proposal.step.size();
    std::memcpy(intent.step_key, proposal.step.data(), proposal.step.size());
    decision_->output->intent_count = 1;
}
// Close and wait refer to an existing Task-owned step, never a new route.
// Capturing the frontier within decide cannot race with owner sample application.
ayni_status_t CallRuntime::stage_subscription_target(const ayni_call_request_v1 &request) {
    if (request.binding_version != 0 || request.binding_id.size != 0 || request.text.size != 0 ||
        request.arguments.struct_size != 0 || request.arguments.field_count != 0 ||
        request.arguments.fields != nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    Proposal proposal{};
    if (!proposal.step.assign(request.step_key)) {
        return AYNI_INVALID_ARGUMENT;
    }
    uint32_t index = 0;
    const auto status = subscription_step(request.step_key, index);
    if (status != AYNI_OK) {
        return status;
    }
    proposal.task_id = work_.tasks.at(decision_->task_index).task_id;
    proposal.subscription_index = index;
    if (request.kind == AYNI_POLICY_WAIT_SUBSCRIPTION) {
        proposal.wait_basis = subscription_wait_basis(index);
    }
    proposal.kind = request.kind;
    stage_intent(proposal);
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
    if (!task_content_current({runtime.work_, runtime.host_.monotonic_now_us(runtime.host_.ctx),
                               runtime.decision_->durable_seq},
                              runtime.decision_->task_index)) {
        return AYNI_EXPIRED;
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
            state.subscription ? state.subscription->generation
                               : calls.identity(index).operation_id,
            state.outcome == 0U ? 0U : state.latest.transition.commit_seq,
            state.status,
            state.reason,
            calls.result(index),
            work::tool_call_count(calls.tool_calls(index)),
            0,
            static_cast<uint32_t>(calls.effect(index)),
            0};
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
                                                  uint64_t now_us, uint64_t durable_seq,
                                                  knowledge::MemoryDependencies &dependencies) {
    const auto &task = work_.tasks.at(task_index);
    if (!proposal_ || proposal_->task_id != task.task_id || proposal_->kind != intent.kind ||
        !proposal_->step.equals({intent.step_key, intent.step_key_size})) {
        return {AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_INTENT};
    }
    const auto &proposal = *proposal_;
    if (proposal.subscription_index) {
        return !proposal.wait_basis || work::TaskTable::can_wait_subscription(task)
                   ? authority::GrantCheck{AYNI_OK, AYNI_REASON_NONE}
                   : authority::GrantCheck{AYNI_CAPACITY_EXCEEDED, AYNI_REASON_REVISION_EXHAUSTED};
    }

    const auto &contract = catalogs_.call_capabilities.at(proposal.links.call).contract;
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
    if (contract.subscription) {
        for (uint32_t at = 0; at < work_.calls.capacity(); ++at) {
            if (!work_.calls.occupied(at) || work_.calls.identity(at).task_id != task.task_id) {
                continue;
            }
            const auto &saved = work_.calls.contract(at).subscription;
            const auto &state = work_.calls.state(at).subscription;
            if (saved && state && state->desired == work::SubscriptionDesired::open &&
                saved->source.equals(view(contract.subscription->source))) {
                return {AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_INTENT};
            }
        }
    }
    const auto domain = catalogs_.capabilities.domain_view(proposal.links.capability);
    work::CallInputView input{};
    if (work::read_call_input(domain, {staging_, proposal.input_size}, contract.input_text_limit,
                              input) != AYNI_OK) {
        return {AYNI_INTERNAL, AYNI_REASON_NONE};
    }
    const auto authority = check_input(task_index, catalogs_.bindings.at(proposal.binding_index),
                                       domain, input, now_us, durable_seq);
    if (authority.status != AYNI_OK) {
        return authority;
    }
    const auto exported =
        memory_export_permissions({work_, now_us, durable_seq}, task.owner_instance_id,
                                  catalogs_.bindings.at(proposal.binding_index), dependencies);
    if (exported != AYNI_OK) {
        return {exported, memory_reason(exported)};
    }
    return control_.prepare(contract, catalogs_.bindings.at(proposal.binding_index),
                            input.arguments, now_us, durable_seq, task.task_id,
                            proposal_->execution);
}

// Exact capacity and identity preflights have succeeded. Any refusal here is an
// internal invariant failure; no callback can intervene between check and apply.
bool CallRuntime::accept(uint32_t task_index, foundation::TransitionStamp stamp, uint64_t now_us) {
    const auto &proposal = *proposal_;
    // Waiting consumes a Task transition and checkpoint, not another call,
    // resource acquisition, model charge or step identity (C-SUB-005).
    if (proposal.wait_basis) {
        work_.tasks.wait_subscription(task_index, *proposal.wait_basis, stamp);
        proposal_.reset();
        return true;
    }
    // The producer and cache projections close with the explicit stop fact,
    // while the original Provider borrow remains owned until terminal receipt.
    if (proposal.subscription_index) {
        const auto index = *proposal.subscription_index;
        if (work_.calls.stop_subscription(index, work::CallCancelCause::user,
                                          work::SubscriptionDesired::closed,
                                          {work_.ids.boot_epoch(), stamp}) != AYNI_OK) {
            return false;
        }
        close_subscription_gate(index);
        work_.tasks.progress(task_index, stamp);
        proposal_.reset();
        return true;
    }

    const auto &binding = catalogs_.bindings.at(proposal.binding_index);
    const auto &contract = catalogs_.call_capabilities.at(proposal.links.call).contract;
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
            identity, contract, catalogs_.capabilities.domain_view(proposal.links.capability),
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
