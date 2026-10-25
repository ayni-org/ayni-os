#include "kernel/call/call_runtime.h"

namespace ayni::kernel {
namespace {
ayni_string_v1 view(const catalog::CatalogId &id) {
    return {id.data(), id.size()};
}
} // namespace

// Save the exact enabled Scene generation and measured prerequisites alongside
// the canonical call input. No permit or local nonce is issued during admission.
authority::GrantCheck CallRuntime::prepare_execution(Proposal &proposal, ayni_bytes_v1 arguments,
                                                     uint64_t now_us, uint64_t durable_seq) const {
    const auto &contract = catalogs_.capabilities.at(proposal.links.call).contract;
    if ((contract.effects & catalog::call_effect_physical) == 0U) {
        return {AYNI_OK, AYNI_REASON_NONE};
    }
    const auto &binding = catalogs_.bindings.at(proposal.binding_index);
    uint32_t mapping = 0;
    uint32_t executor = 0;
    if (!work_.execution_catalog.find_binding(view(binding.id), binding.version, mapping)) {
        return {AYNI_UNSUPPORTED, AYNI_REASON_INTENT_UNSUPPORTED};
    }
    const auto &spec = work_.execution_catalog.binding(mapping);
    uint32_t scene = 0;
    const auto authorized =
        work_.scenes.authorize(view(spec.scene), spec.scene_version, view(binding.capability),
                               binding.capability_version, arguments, durable_seq, scene);
    if (authorized != AYNI_OK) {
        return {authorized, AYNI_REASON_EXECUTION_SCENE};
    }
    uint32_t state = 0;
    if (!work_.scenes.find_state(view(spec.scene), state) ||
        !work_.execution_catalog.find_executor(view(spec.executor), spec.executor_version,
                                               executor) ||
        work_.principals.binding() == nullptr) {
        return {AYNI_INTERNAL, AYNI_REASON_NONE};
    }
    const auto &ports = work_.execution_catalog.executor(executor).ports;
    const auto &definition = work_.scenes.definition(scene);
    work::ExecutionFact fact{};
    fact.binding = spec;
    fact.scene_seq = work_.scenes.state(state).stamp.commit_seq;
    fact.control_domain = ports.control_domain_id;
    fact.control_mode = ports.control_mode;
    fact.epochs = {work_.ids.boot_epoch(),    work_.principals.binding()->binding_revision,
                   ports.executor_boot_epoch, ports.safety_epoch,
                   ports.calibration_version, 0};
    fact.dependency_count = definition.condition_count;
    for (uint32_t at = 0; at < fact.dependency_count; ++at) {
        const auto status = observations_.require(definition.conditions[at].predicate, now_us,
                                                  fact.dependencies[at]);
        if (status != AYNI_OK) {
            return {status, AYNI_REASON_EXECUTION_OBSERVATION};
        }
    }
    if (!work::valid_execution(fact, arguments)) {
        return {AYNI_INVALID_ARGUMENT, AYNI_REASON_GRANT_INPUT};
    }
    proposal.execution = fact;
    return {AYNI_OK, AYNI_REASON_NONE};
}

// Used immediately before dispatch and every local renewal. Scene close/reopen,
// a new observation version or a new boot cannot revive an accepted old action.
bool CallRuntime::execution_allowed(uint32_t index, uint64_t now_us, uint64_t durable_seq) const {
    const auto &saved = work_.calls.state(index).execution;
    if (!saved) {
        return true;
    }
    const auto &fact = *saved;
    const auto &identity = work_.calls.identity(index);
    const auto *owner = work_.principals.binding();
    if (work::execution_stopped(fact) || fact.epochs.kernel_boot_epoch != work_.ids.boot_epoch() ||
        owner == nullptr || fact.epochs.authority_epoch != owner->binding_revision) {
        return false;
    }
    work::CallInputView input{};
    if (work::read_call_input(work_.calls.domain(index), work_.calls.input(index),
                              work_.calls.contract(index).input_text_limit, input) != AYNI_OK) {
        return false;
    }
    uint32_t scene = 0;
    uint32_t state = 0;
    if (work_.scenes.authorize(view(fact.binding.scene), fact.binding.scene_version,
                               view(identity.capability_id), identity.capability_version,
                               input.arguments, durable_seq, scene) != AYNI_OK ||
        !work_.scenes.find_state(view(fact.binding.scene), state) ||
        work_.scenes.state(state).stamp.commit_seq != fact.scene_seq) {
        return false;
    }
    return execution_dependencies_current(index, now_us);
}

// An output can record that execution ended, but stale evidence cannot complete
// a dependent step or contribute its old body to the next strategy context.
bool CallRuntime::execution_dependencies_current(uint32_t index, uint64_t now_us) const {
    const auto &saved = work_.calls.state(index).execution;
    if (!saved) {
        return true;
    }
    const auto &fact = *saved;
    uint32_t scene = 0;
    if (!work_.scenes.find_definition(view(fact.binding.scene), scene)) {
        return false;
    }
    const auto &definition = work_.scenes.definition(scene);
    if (definition.version != fact.binding.scene_version ||
        definition.condition_count != fact.dependency_count) {
        return false;
    }
    for (uint32_t at = 0; at < fact.dependency_count; ++at) {
        ayni_observation_ref_v1 fresh{};
        if (!observations_.current(fact.dependencies[at], now_us) ||
            observations_.require(definition.conditions[at].predicate, now_us, fresh) != AYNI_OK) {
            return false;
        }
    }
    return true;
}

work::ResultDependencies CallRuntime::result_dependencies(uint32_t index) const {
    return execution_dependencies_current(index, host_.monotonic_now_us(host_.ctx))
               ? work::ResultDependencies::current
               : work::ResultDependencies::stale;
}

// The saved DISPATCHING barrier is already durable. A fresh challenge anchors
// the executor's deadline, and the full envelope is derived from authorized bytes.
ayni_status_t CallRuntime::issue_permit(uint32_t index, ayni_bytes_v1 arguments, uint64_t now_us) {
    const auto &saved = work_.calls.state(index).execution;
    if (!saved) {
        return AYNI_OK;
    }
    const auto &fact = *saved;
    uint32_t executor = 0;
    if (!work_.execution_catalog.find_executor(view(fact.binding.executor),
                                               fact.binding.executor_version, executor)) {
        return AYNI_NOT_FOUND;
    }
    if (!permits_.available(executor)) {
        return AYNI_BUSY;
    }
    const auto &ports = work_.execution_catalog.executor(executor).ports;
    ayni_executor_nonce_v1 nonce{};
    work_.calling_out = true;
    const auto status = ports.challenge(ports.ctx, &fact.epochs, &nonce);
    work_.calling_out = false;
    if (status != AYNI_OK) {
        return status;
    }
    if (nonce.control_domain_id != fact.control_domain || nonce.nonce == 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &id = work_.calls.identity(index);
    // Acceptance IDs may be reordered by priority/RR. The durable dispatch attempt
    // is assigned after exclusive acquisition, so each executor sees increasing permit IDs.
    ayni_execution_permit_v1 permit{sizeof(permit),
                                    fact.binding.form,
                                    work_.calls.state(index).attempt.request_id,
                                    id.operation_id,
                                    fact.control_domain,
                                    fact.epochs,
                                    nonce.nonce,
                                    fact.control_mode,
                                    fact.binding.lease_us,
                                    fact.binding.max_command_age_us,
                                    0,
                                    {}};
    if (catalog::execution_envelope(fact.binding, arguments, permit.envelope) != AYNI_OK) {
        return AYNI_INTERNAL;
    }
    return permits_.install(id.call_id, executor, slots_[index].active->claim, permit, now_us)
               ? AYNI_OK
               : AYNI_INTERNAL;
}

// Durable work facts retain measured stop independently of Provider completion.
// Resource quarantine is lifted only by the independent consumer after this stamp persists.
CallRuntime::Advance CallRuntime::commit_stop(uint32_t index) {
    const auto *lease = permits_.find(work_.calls.identity(index).call_id);
    if (lease == nullptr || !lease->stopped) {
        return Advance::internal;
    }
    const auto &fact = *lease->stopped;
    return work_.calls.confirm_execution(index, fact.phase, fact.cause, fact.target,
                                         {work_.ids.boot_epoch(), work_.transitions.next()}) ==
                   AYNI_OK
               ? Advance::applied
               : Advance::internal;
}

// The ledger cannot retire while either a transport borrow or a local lease survives.
bool CallRuntime::runtime_retains(const void *context, uint64_t call_id) {
    const auto &runtime = *static_cast<const CallRuntime *>(context);
    if (runtime.permits_.find(call_id) != nullptr) {
        return true;
    }
    for (uint32_t at = 0; at < runtime.limits_.capacity; ++at) {
        if (runtime.slots_[at].active && runtime.slots_[at].active->call_id == call_id) {
            return true;
        }
    }
    return false;
}
ProviderMailbox::BlockedSlots CallRuntime::blocked_receipts() const {
    ProviderMailbox::BlockedSlots blocked{};
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        const auto &active = slots_[at].active;
        if (active && active->stage == Stage::waiting_stop && active->delivery &&
            work_.calls.state(at).execution &&
            !work::execution_stopped(*work_.calls.state(at).execution)) {
            blocked[active->delivery->reservation.index] = true;
        }
    }
    return blocked;
}
} // namespace ayni::kernel
