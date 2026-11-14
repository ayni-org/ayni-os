#include "foundation/workspace.h"
#include "kernel/service/call/call_runtime.h"
#include "kernel/service/memory/memory_permissions.h"

#include "authority/scope/business_request.h"
#include "catalog/abi_checks.h"
#include "catalog/capability/business_encoding.h"
#include "foundation/deadline.h"

#include <cstring>

namespace ayni::kernel {
namespace {
constexpr uint32_t identities_per_call =
    2; // Logical call and operation, both stable before dispatch.

ayni_string_v1 view(const catalog::CatalogId &id) {
    return {id.data(), id.size()};
}

// Validate the ABI header before selecting a subscription or ordinary Call path.
ayni_status_t request_header(const ayni_call_request_v1 &request) {
    const auto shape =
        catalog::check_struct_size(request.struct_size, sizeof(request), sizeof(request));
    if (shape != AYNI_OK) {
        return shape;
    }
    if (request.reserved != 0U || request.mode_reserved != 0 ||
        request.mode > AYNI_CALL_MODE_MANAGED ||
        (request.kind != AYNI_POLICY_REQUEST_CALL && request.kind != AYNI_POLICY_REQUEST_MODEL &&
         request.kind != AYNI_POLICY_CLOSE_SUBSCRIPTION &&
         request.kind != AYNI_POLICY_WAIT_SUBSCRIPTION)) {
        return AYNI_INVALID_ARGUMENT;
    }
    return AYNI_OK;
}

} // namespace

void CallRuntime::begin_decision(uint32_t task_index, ayni_task_snapshot_v1 &snapshot,
                                 ayni_policy_output_v1 &output, uint64_t durable_seq) {
    proposal_.reset();
    if (limits_.capacity == 0U) {
        return;
    }
    decision_ = Decision{task_index, durable_seq, snapshot.now_us, {}, output.intents, &output};
    output.call_context = this;
    output.request_call = stage_request;
    output.control_operation = stage_operation_control;
    snapshot.call_context = this;
    snapshot.call_result = read_result;
    snapshot.list_capabilities = list_capabilities_view;
    snapshot.inspect_capability = inspect_capability_view;
    snapshot.capability_context = capability_context_view;
    snapshot.subscription = read_subscription;
    snapshot.subscription_observation = read_subscription_observation;
    snapshot.operation = read_operation;
}

void CallRuntime::end_decision() {
    // Reads after request_call still contribute to this decision's egress proof.
    // Business execution authority stays independent of metadata disclosure.
    if (proposal_ && proposal_->kind == AYNI_POLICY_REQUEST_MODEL && decision_) {
        proposal_->model_context = decision_->model_context;
    }
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
    const auto header = request_header(request);
    if (header != AYNI_OK) {
        return header;
    }
    if (proposal_) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    if (request.kind == AYNI_POLICY_CLOSE_SUBSCRIPTION ||
        request.kind == AYNI_POLICY_WAIT_SUBSCRIPTION) {
        return stage_subscription_target(request);
    }
    const auto &task = work_.tasks.at(decision_->task_index);
    const auto &policy = work_.goals.at(task.goal_index).tools;
    if (request.kind == AYNI_POLICY_REQUEST_CALL && policy.authorization == AYNI_TOOLS_ON_DEMAND &&
        !policy.contains(request.binding_id, request.binding_version)) {
        return AYNI_NOT_FOUND;
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
    return stage_business(request, proposal);
}

// Normalize once against the shared machine proof, then apply fresh optional-tool authority.
ayni_status_t CallRuntime::stage_business(const ayni_call_request_v1 &request, Proposal &proposal) {
    const auto &task = work_.tasks.at(decision_->task_index);
    const auto &binding = catalogs_.bindings.at(proposal.binding_index);
    const auto &contract = proposal.links.contract;
    const auto form = check_call_form(task.task_id, request, contract);
    if (form != AYNI_OK) {
        return form;
    }
    proposal.mode = request.mode;
    if ((request.arguments.struct_size != 0U || request.arguments.field_count != 0U ||
         request.arguments.fields != nullptr || request.text.size != 0U)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto domain = catalogs_.capabilities.domain_view(proposal.links.capability);
    auto input_lease = work_.workspace.make<work::CallInputView>();
    auto proof_lease = work_.workspace.make<catalog::DecodedBusiness>();
    if (!input_lease || !proof_lease) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto &input = *input_lease;
    const auto *proof = &*proof_lease;
    auto status =
        catalog::decode_business(work_.workspace, domain, contract.business_contract, *proof_lease);
    if (status != AYNI_OK) {
        return status;
    }
    status = work::encode_business_input_with_proof(work_.workspace, *proof, contract,
                                                    request.value, staging_, limits_.input_bytes,
                                                    proposal.input_size, &input.value);
    if (status != AYNI_OK) {
        return status;
    }
    if (contract.category == catalog::CallClass::tool) {
        if (on_demand_tool(decision_->task_index, binding, contract) &&
            !work_.task_authority.can_bind_tool(task.slot)) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        // A complete model proposal can receive a normal tool refusal before
        // the sink is consumed. Final preflight still rechecks live authority.
        const auto checked =
            check_input(decision_->task_index, binding, domain, input, contract, *proof,
                        decision_->now_us, decision_->durable_seq, 0, &proposal.authorization);
        if (checked.status != AYNI_OK) {
            return checked.status;
        }
    }
    proposal.task_id = work_.tasks.at(decision_->task_index).task_id;
    proposal.kind = request.kind;
    if (request.kind == AYNI_POLICY_REQUEST_MODEL) {
        proposal.model_context = decision_->model_context;
    }
    stage_intent(proposal);
    return AYNI_OK;
}
// Mode/category and single-action occupancy are one admission rule for every tool route.
ayni_status_t CallRuntime::check_call_form(uint64_t task_id, const ayni_call_request_v1 &request,
                                           const catalog::CallContract &contract) const {
    if (request.mode == AYNI_CALL_MODE_MANAGED && (contract.category != catalog::CallClass::tool ||
                                                   contract.form == catalog::CallForm::unary)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (contract.category == catalog::CallClass::tool &&
        contract.form == catalog::CallForm::action && action_occupied(task_id)) {
        return AYNI_BUSY;
    }
    if ((contract.category == catalog::CallClass::model) !=
        (request.kind == AYNI_POLICY_REQUEST_MODEL)) {
        return AYNI_INVALID_ARGUMENT;
    }
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
    if (request.mode != AYNI_CALL_MODE_FOREGROUND || request.binding_version != 0 ||
        request.binding_id.size != 0 || request.text.size != 0 ||
        request.arguments.struct_size != 0 || request.arguments.field_count != 0 ||
        request.arguments.fields != nullptr || request.value.node_count != 0U ||
        request.value.nodes != nullptr) {
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
    if (context == nullptr || out == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape = catalog::check_struct_size(out->struct_size, sizeof(*out), sizeof(*out));
    if (shape != AYNI_OK) {
        return shape;
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
    // An inline proposal list is handed over whole, like a body the store keeps (ADR-0096).
    const auto list = calls.tool_calls(index);
    const uint32_t inline_calls =
        list.size != 0U ? work::tool_call_count(runtime.work_.workspace, list) : 0U;
    const ayni_string_v1 text =
        list.size != 0U ? ayni_string_v1{reinterpret_cast<const char *>(list.data), list.size}
                        : calls.result(index);
    *out = {sizeof(*out),
            state.outcome,
            calls.identity(index).call_id,
            state.subscription ? state.subscription->generation
                               : calls.identity(index).operation_id,
            state.outcome == 0U ? 0U : state.latest.transition.commit_seq,
            state.status,
            state.reason,
            text,
            state.result_body.id != 0U ? state.result_body_calls : inline_calls,
            0,
            static_cast<uint32_t>(calls.effect(index)),
            0,
            calls.value_result(index),
            calls.domain(index),
            calls.contract(index).business_contract,
            state.model_usage,
            calls.identity(index).mode,
            0,
            state.result_body};
    if (calls.record(index).result_kind == work::CallResultKind::unavailable ||
        !runtime.retain_model_context(
            runtime.decision_->task_index, calls.identity(index).model_context,
            runtime.host_.monotonic_now_us(runtime.host_.ctx), runtime.decision_->durable_seq)) {
        // ABI 0.50: preserve known identities/outcome/effect while withholding body.
        out->text = {};
        out->encoded_value = {};
        out->tool_call_count = 0;
        out->body = {};
        return AYNI_EXPIRED;
    }
    return AYNI_OK;
}

// Required capabilities keep their immutable Goal ceiling. Other listed tools
// acquire one whole Grant only when their first concrete request is accepted.
bool CallRuntime::on_demand_tool(uint32_t task_index, const catalog::Binding &binding,
                                 const catalog::CallContract &contract) const {
    const auto &task = work_.tasks.at(task_index);
    if (contract.category != catalog::CallClass::tool ||
        work_.goals.at(task.goal_index).tools.authorization != AYNI_TOOLS_ON_DEMAND) {
        return false;
    }
    const auto ceiling = work_.goal_authority.at(task.goal_index);
    for (uint32_t at = 0; at < ceiling.count; ++at) {
        const auto &need = ceiling.requirements[at];
        if (need.capability_id.equals(view(binding.capability)) &&
            need.capability_version == binding.capability_version) {
            return false;
        }
    }
    return true;
}

// An admitted optional Call always reuses its Task's exact current selection.
// New requests may choose a Grant; queued delivery and renewal cannot substitute.
authority::GrantCheck
CallRuntime::check_input(uint32_t task_index, const catalog::Binding &binding, ayni_bytes_v1 domain,
                         const work::CallInputView &input, const catalog::CallContract &contract,
                         const catalog::DecodedBusiness &proof, uint64_t now_us,
                         uint64_t durable_seq, uint64_t source_call_id,
                         work::AuthorityReference *selected) const {
    const auto authority = work_.check_task_authority(task_index, now_us, durable_seq);
    if (authority.status != AYNI_OK) {
        return authority;
    }
    const auto policy = check_business_proof(task_index, binding, domain, input.value.view(),
                                             contract, proof.view());
    if (policy.status != AYNI_OK || !on_demand_tool(task_index, binding, contract)) {
        return policy;
    }
    const auto &task = work_.tasks.at(task_index);
    if (source_call_id) {
        const auto saved = work_.task_authority.at(task.slot);
        for (uint32_t at = 0; at < saved.tool_count; ++at) {
            if (saved.tools[at].source_call_id == source_call_id) {
                return work_.check_tool_authority(task_index, saved.tools[at], now_us, durable_seq);
            }
        }
        return {AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_MISSING};
    }
    std::optional<uint32_t> candidate;
    for (uint32_t at = 0; at < work_.grants.size(); ++at) {
        const auto &grant = work_.grants.at(at);
        if (grant.subject_instance_id != task.owner_instance_id ||
            !work_.grants.covers_saved_scope(at, binding.capability, binding.capability_version,
                                             domain, work_.grants.scope(at),
                                             contract.business_contract) ||
            work_.check_grant(at, now_us, durable_seq, task.goal_id).status != AYNI_OK) {
            continue;
        }
        const auto covered = authority::contains_business_request(
            binding, domain, proof.view(), input.value.view(), work_.grants.scope(at));
        if (covered.status != AYNI_OK || !covered.covered) {
            continue;
        }
        if (!candidate || authority::prefer_admitted_grant(grant, work_.grants.at(*candidate))) {
            candidate = at;
        }
    }
    if (!candidate) {
        return {AYNI_DENIED, AYNI_REASON_GRANT_SCOPE};
    }
    if (selected) {
        const auto &grant = work_.grants.at(*candidate);
        *selected = {grant.grant_id, grant.revision};
    }
    return {AYNI_OK, AYNI_REASON_NONE};
}

// Every ancestor retains its own policy. An existing matching requirement is
// restrictive even in lazy mode: failure cannot fall through to another Grant.
authority::GrantCheck
CallRuntime::check_business_proof(uint32_t task_index, const catalog::Binding &binding,
                                  ayni_bytes_v1 domain, const ayni_value_v1 &value,
                                  const catalog::CallContract &contract,
                                  const ayni_business_contract_v1 &proof) const {
    const auto &task = work_.tasks.at(task_index);
    if (contract.category == catalog::CallClass::tool &&
        !work_.tool_policy_allows(task.goal_index, binding)) {
        return {AYNI_DENIED, AYNI_REASON_GRANT_SCOPE};
    }
    for (uint32_t goal_index = task.goal_index; goal_index != work::GoalTable::no_parent;
         goal_index = work_.goals.at(goal_index).parent_index) {
        const auto ceiling = work_.goal_authority.at(goal_index);
        bool matching = false;
        bool covered = false;
        for (uint32_t at = 0; at < ceiling.count && !covered; ++at) {
            const auto &need = ceiling.requirements[at];
            if (!need.capability_id.equals(view(binding.capability)) ||
                need.capability_version != binding.capability_version) {
                continue;
            }
            matching = true;
            if (need.source_domain.size != domain.size ||
                std::memcmp(need.source_domain.data, domain.data, domain.size) != 0 ||
                need.business_contract.size != contract.business_contract.size ||
                std::memcmp(need.business_contract.data, contract.business_contract.data,
                            contract.business_contract.size) != 0) {
                continue;
            }
            const auto relation =
                authority::contains_business_request(binding, domain, proof, value, need.scope);
            covered = relation.status == AYNI_OK && relation.covered;
        }
        const bool optional =
            contract.category == catalog::CallClass::tool &&
            work_.goals.at(goal_index).tools.authorization == AYNI_TOOLS_ON_DEMAND;
        if (!covered && (matching || !optional)) {
            return {AYNI_DENIED, AYNI_REASON_GRANT_SCOPE};
        }
    }
    return {AYNI_OK, AYNI_REASON_NONE};
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
    auto &proposal = *proposal_;
    if (proposal.operation_index) {
        return (!proposal.wait_basis || work_.tasks.can_wait_with_calls(task)) &&
                       work_.tasks.has_revisions(task, 1U) &&
                       work_.accepted_step_count(task_index) < work_.tasks.step_capacity() &&
                       !work_.step_taken(task_index, view(proposal.step))
                   ? authority::GrantCheck{AYNI_OK, AYNI_REASON_NONE}
                   : authority::GrantCheck{AYNI_CAPACITY_EXCEEDED, AYNI_REASON_POLICY_OUTPUT_LIMIT};
    }
    if (proposal.subscription_index) {
        return !proposal.wait_basis || work_.tasks.can_wait_with_calls(task)
                   ? authority::GrantCheck{AYNI_OK, AYNI_REASON_NONE}
                   : authority::GrantCheck{AYNI_CAPACITY_EXCEEDED, AYNI_REASON_REVISION_EXHAUSTED};
    }

    if (!model_context_current(task_index, proposal.model_context, now_us, durable_seq)) {
        // Metadata expired during decide. Rebuild before accepting input or checkpoint.
        proposal.redecide = true;
        return {AYNI_EXPIRED, AYNI_REASON_NONE};
    }
    const auto &contract = proposal.links.contract;
    if (contract.category == catalog::CallClass::model && task.commit_seq > durable_seq) {
        proposal.redecide = true;
        return {
            AYNI_BUSY,
            AYNI_REASON_NONE}; // Handles/controls cannot be externally visible before durability.
    }
    const auto capacity = check_call_capacity(task_index, proposal, contract);
    if (capacity.status != AYNI_OK) {
        return capacity;
    }
    const auto domain = catalogs_.capabilities.domain_view(proposal.links.capability);
    auto input_lease = work_.workspace.make<work::CallInputView>();
    if (!input_lease) {
        return {AYNI_CAPACITY_EXCEEDED, AYNI_REASON_NONE};
    }
    auto &input = *input_lease;
    auto proof_lease = work_.workspace.make<catalog::DecodedBusiness>();
    if (!proof_lease ||
        catalog::decode_business(work_.workspace, domain, contract.business_contract,
                                 *proof_lease) != AYNI_OK ||
        work::read_call_input_with_proof(work_.workspace, *proof_lease, domain,
                                         {staging_, proposal.input_size}, contract,
                                         input) != AYNI_OK) {
        return {AYNI_INTERNAL, AYNI_REASON_NONE};
    }
    const auto *proof = &*proof_lease;
    const auto authority =
        check_input(task_index, catalogs_.bindings.at(proposal.binding_index), domain, input,
                    contract, *proof, now_us, durable_seq, 0, &proposal.authorization);
    if (authority.status != AYNI_OK) {
        // An unused optional Grant can expire during decide. Discard the proposal
        // and checkpoint; the next callback observes refusal before consuming its sink.
        proposal.redecide =
            proposal.authorization.grant_id != 0 &&
            work_.check_task_authority(task_index, now_us, durable_seq).status == AYNI_OK;
        return authority;
    }
    const auto exported =
        memory_export_permissions({work_, now_us, durable_seq}, task.owner_instance_id,
                                  catalogs_.bindings.at(proposal.binding_index), dependencies);
    if (exported != AYNI_OK) {
        return {exported, memory_reason(exported)};
    }
    return control_.prepare(contract, catalogs_.bindings.at(proposal.binding_index), domain, input,
                            *proof, now_us, durable_seq, task.task_id, proposal_->execution);
}

// All bounded storage, lifecycle reserve and unique-source checks precede acceptance.
authority::GrantCheck
CallRuntime::check_call_capacity(uint32_t task_index, const Proposal &proposal,
                                 const catalog::CallContract &contract) const {
    const auto &task = work_.tasks.at(task_index);
    if (task.call.id != 0U || work_.step_taken(task_index, view(proposal.step))) {
        return {AYNI_INVALID_ARGUMENT, AYNI_REASON_STEP_TAKEN};
    }
    const bool detaches =
        proposal.mode == AYNI_CALL_MODE_MANAGED || contract.subscription.has_value();
    if (detaches ? !work_.tasks.can_begin_background_call(task)
                 : !work_.tasks.can_begin_with_calls(task)) {
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
    return {AYNI_OK, AYNI_REASON_NONE};
}

// Exact capacity and identity preflights have succeeded. Any refusal here is an
// internal invariant failure; no callback can intervene between check and apply.
bool CallRuntime::accept(uint32_t task_index, foundation::TransitionStamp stamp, uint64_t now_us) {
    const auto &proposal = *proposal_;
    if (proposal.operation_index || proposal.wait_basis || proposal.subscription_index) {
        return accept_control(task_index, stamp, now_us);
    }
    const auto &binding = catalogs_.bindings.at(proposal.binding_index);
    const auto &contract = proposal.links.contract;
    const auto &task = work_.tasks.at(task_index);
    work::CallIdentity identity{};
    if (!work_.ids.allocate(identity.call_id) || !work_.ids.allocate(identity.operation_id)) {
        return false;
    }
    identity.authorization = proposal.authorization;
    identity.mode = proposal.mode;
    identity.model_context = proposal.model_context;
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
    if (identity.mode == AYNI_CALL_MODE_MANAGED) {
        // The step records acceptance, never business success. The original Call owns its result.
        if (!work_.tasks.add_step(task_index, view(identity.step_key), stamp)) {
            return false;
        }
        work_.tasks.progress(task_index, stamp);
    } else {
        work_.tasks.begin_call(task_index, identity.call_id,
                               contract.category == catalog::CallClass::model
                                   ? work::TaskTable::CallKind::model
                                   : work::TaskTable::CallKind::tool,
                               stamp);
    }
    if (identity.authorization.grant_id) {
        work_.task_authority.bind_tool_validated(task.slot,
                                                 {identity.call_id, identity.authorization}, stamp);
    }
    proposal_.reset();
    return true;
}

// Control changes only existing rows/waits; start allocation remains in accept().
bool CallRuntime::accept_control(uint32_t task_index, foundation::TransitionStamp stamp,
                                 uint64_t now_us) {
    const auto &proposal = *proposal_;
    if (proposal.operation_index) {
        const auto index = *proposal.operation_index;
        if (!work_.tasks.add_step(task_index, view(proposal.step), stamp)) {
            return false;
        }
        if (proposal.wait_basis) {
            auto wait = *proposal.wait_basis;
            wait.deadline_us = foundation::deadline(now_us, uint64_t{wait.timeout_ms} *
                                                                microseconds_per_millisecond);
            work_.tasks.wait_operation(task_index, wait, stamp);
        } else {
            const auto action_stamp = work::ActionStamp{work_.ids.boot_epoch(), stamp};
            const auto status =
                proposal.control_kind == AYNI_OPERATION_CLOSE
                    ? work_.calls.stop_subscription(index, work::CallCancelCause::user,
                                                    work::SubscriptionDesired::closed, action_stamp)
                    : work_.calls.request_cancel(index, work::CallCancelCause::user, action_stamp);
            if (status != AYNI_OK) {
                return false;
            }
            if (proposal.control_kind == AYNI_OPERATION_CLOSE) {
                close_subscription_gate(index);
            }
            work_.tasks.progress(task_index, stamp);
        }
        proposal_.reset();
        return true;
    }
    // Waiting consumes a Task transition and checkpoint, not another call,
    // resource acquisition, model charge or step identity (C-SUB-005).
    if (proposal.wait_basis) {
        work_.tasks.wait_operation(task_index, *proposal.wait_basis, stamp);
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

    return false; // Caller selected only the three supported existing-operation controls.
}

// The original ledger and runtime borrow, not a duplicated Task counter, own occupancy.
bool CallRuntime::action_occupied(uint64_t task_id) const {
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        if (!work_.calls.occupied(at) || work_.calls.identity(at).task_id != task_id) {
            continue;
        }
        const auto &contract = work_.calls.contract(at);
        if (contract.category == catalog::CallClass::tool &&
            contract.form == catalog::CallForm::action &&
            (work_.calls.state(at).outcome == 0 || work_.calls.unresolved(at) ||
             slots_[at].active)) {
            return true;
        }
    }
    return false;
}
} // namespace ayni::kernel
