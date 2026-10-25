#include "kernel/execution/execution_control.h"

#include "foundation/deadline.h"

namespace ayni::kernel {
namespace {
ayni_string_v1 view(const catalog::CatalogId &id) {
    return {id.data(), id.size()};
}
} // namespace

// Save the exact enabled Scene generation and measured prerequisites alongside
// the canonical call input. No permit or local nonce is issued during admission.
authority::GrantCheck
ExecutionControl::prepare(const catalog::CallContract &contract, const catalog::Binding &binding,
                          ayni_bytes_v1 arguments, uint64_t now_us, uint64_t durable_seq,
                          std::optional<work::ExecutionFact> &out_fact) const {
    if ((contract.effects & catalog::call_effect_physical) == 0U) {
        return {AYNI_OK, AYNI_REASON_NONE};
    }
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
    out_fact = fact;
    return {AYNI_OK, AYNI_REASON_NONE};
}

// Used immediately before dispatch and every local renewal. Scene close/reopen,
// a new observation version or a new boot cannot revive an accepted old action.
bool ExecutionControl::allowed(uint32_t index, uint64_t now_us, uint64_t durable_seq) const {
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
    return dependencies_current(index, now_us);
}

// An output can record that execution ended, but stale evidence cannot complete
// a dependent step or contribute its old body to the next strategy context.
bool ExecutionControl::dependencies_current(uint32_t index, uint64_t now_us) const {
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

work::ResultDependencies ExecutionControl::result_dependencies(uint32_t index) const {
    return dependencies_current(index, host_.monotonic_now_us(host_.ctx))
               ? work::ResultDependencies::current
               : work::ResultDependencies::stale;
}

// The saved DISPATCHING barrier is already durable. A fresh challenge anchors
// the executor's deadline, and the full envelope is derived from authorized bytes.
ayni_status_t ExecutionControl::issue(uint32_t index, const resources::ResourceClaim &claim,
                                      ayni_bytes_v1 arguments, uint64_t now_us) {
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
    const auto status =
        work_.call_out([&] { return ports.challenge(ports.ctx, &fact.epochs, &nonce); });
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
    return permits_.install(id.call_id, executor, claim, permit, now_us) ? AYNI_OK : AYNI_INTERNAL;
}

const ayni_execution_permit_v1 *ExecutionControl::permit(uint64_t call_id) const {
    const auto *lease = permits_.find(call_id);
    return lease == nullptr ? nullptr : &lease->permit;
}

bool ExecutionControl::delivery_ended(uint64_t call_id) {
    auto *lease = permits_.find(call_id);
    if (lease == nullptr) {
        return false;
    }
    lease->terminal_seen = true;
    return true;
}

bool ExecutionControl::request_stop(uint64_t call_id) {
    return permits_.request_stop(call_id);
}

bool ExecutionControl::stop_measured(uint64_t call_id) const {
    const auto *lease = permits_.find(call_id);
    return lease != nullptr && lease->stopped.has_value();
}

bool ExecutionControl::holds_lease(uint64_t call_id) const {
    return permits_.find(call_id) != nullptr;
}

bool ExecutionControl::idle() const {
    return permits_.empty();
}

uint64_t ExecutionControl::next_deadline_us() const {
    return permits_.next_deadline_us();
}

// Durable work facts retain measured stop independently of Provider completion.
// Resource quarantine is lifted only by the independent consumer after this stamp persists.
RuntimeAdvance ExecutionControl::commit_stop(uint32_t index) {
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

// Lease slots outlive work-model reload. Inhibition never trusts a partially
// restored CallTable index or waits for a new writer to finish recovery.
RuntimeAdvance ExecutionControl::advance_urgent(uint64_t now_us, uint64_t durable_seq,
                                                RuntimeMode mode) {
    for (uint32_t at = 0; at < permits_.capacity(); ++at) {
        const auto *lease = permits_.at(at);
        if (lease == nullptr || lease->stopped) {
            continue;
        }
        const bool issue_stop =
            lease->phase == resources::PermitLease::Phase::issued
                ? should_stop(*lease, now_us, durable_seq, mode)
                : lease->stop_delivery == resources::PermitLease::StopDelivery::pending;
        if (issue_stop) {
            return poll(at, now_us, durable_seq, mode);
        }
    }
    return Advance::idle;
}

// Independent ports never mutate persistent facts. In particular, journal
// backpressure cannot delay disabling output or maintaining actuator quarantine.
ExecutionControl::Advance ExecutionControl::poll(uint32_t index, uint64_t now_us,
                                                 uint64_t durable_seq, Mode mode) {
    auto *lease = permits_.at(index);
    if (lease == nullptr) {
        return Advance::idle;
    }
    if (lease->stopped) {
        if (stopped_releasable(*lease, durable_seq, mode)) {
            const auto call_id = lease->call_id;
            if (!permits_.release_stopped(call_id)) {
                return Advance::internal;
            }
            transport_.retire(transport_.context, call_id, now_us);
            return Advance::applied;
        }
        return Advance::idle;
    }
    const bool allowed = !should_stop(*lease, now_us, durable_seq, mode);
    if (!allowed && !permits_.request_stop(lease->call_id)) {
        return Advance::internal;
    }
    const auto &ports = work_.execution_catalog.executor(lease->executor_index).ports;
    if (lease->phase == resources::PermitLease::Phase::stopping &&
        lease->stop_delivery == resources::PermitLease::StopDelivery::pending) {
        const auto status =
            work_.call_out([&] { return ports.stop(ports.ctx, lease->permit.permit_id); });
        // A not-installed permit remains quarantined until terminal + authoritative absence.
        // Port failure leaves the gate to expire; no return status is physical stop evidence.
        lease->port_status = status;
        lease->stop_delivery = resources::PermitLease::StopDelivery::sent;
        lease->poll_after_us = now_us;
        return Advance::applied;
    }
    if (now_us < lease->poll_after_us) {
        return Advance::idle;
    }
    lease->poll_after_us = foundation::deadline(now_us, ports.poll_period_us);
    ayni_executor_status_v1 status{sizeof(status), AYNI_TARGET_UNVERIFIED, {}};
    status.control.struct_size = sizeof(status.control);
    const auto read =
        work_.call_out([&] { return ports.status(ports.ctx, lease->permit.permit_id, &status); });
    lease->port_status = read;
    if (read == AYNI_NOT_FOUND && lease->terminal_seen) {
        return permits_.not_started(lease->call_id) ? Advance::applied : Advance::internal;
    }
    if (read == AYNI_OK && !permits_.observe(lease->call_id, status)) {
        lease->port_status = AYNI_INVALID_ARGUMENT;
    }
    if (lease->port_status != AYNI_OK) {
        return permits_.request_stop(lease->call_id) ? Advance::applied : Advance::internal;
    }
    if (allowed && status.control.phase == AYNI_CONTROL_ACTIVE &&
        lease->permit.form == AYNI_FORM_SESSION) {
        return renew(*lease);
    }
    return Advance::applied;
}

// Normal operation requires durable stopped evidence. Explicit closing of an
// unwritable Kernel may release only volatile ownership after all borrowers end;
// the saved possible dispatch is left unchanged for later UNKNOWN reconciliation.
bool ExecutionControl::stopped_releasable(const resources::PermitLease &lease, uint64_t durable_seq,
                                          Mode mode) const {
    if (mode == Mode::closing) {
        return transport_.retiring(transport_.context, lease.call_id);
    }
    uint32_t index = 0;
    if (mode == Mode::fenced || !work_.calls.find(lease.call_id, index)) {
        return false;
    }
    const auto &state = work_.calls.state(index);
    return state.execution && work::execution_stopped(*state.execution) &&
           state.latest.transition.commit_seq <= durable_seq;
}

// All execution gates share the same decision for renewal and immediate stopping.
bool ExecutionControl::should_stop(const resources::PermitLease &lease, uint64_t now_us,
                                   uint64_t durable_seq, Mode mode) const {
    if (mode != Mode::running || lease.terminal_seen) {
        return true;
    }
    bool expired = false;
    uint32_t index = 0;
    uint32_t task_index = 0;
    if (!transport_.holds(transport_.context, lease.call_id, now_us, expired) ||
        !work_.calls.find(lease.call_id, index) ||
        !work_.tasks.find(work_.calls.identity(index).task_id, task_index)) {
        return true;
    }
    const auto &task = work_.tasks.at(task_index);
    return expired || task.call.end != work::TaskCall::End::none ||
           task.lifecycle == AYNI_TASK_PAUSED ||
           work_.calls.state(index).cancel_cause != work::CallCancelCause::none ||
           work_.check_task_authority(task_index, now_us, durable_seq).status != AYNI_OK ||
           !allowed(index, now_us, durable_seq) || !resources_.executable(lease.claim);
}

// Renewal uses the original complete generations and a new executor-local nonce.
ExecutionControl::Advance ExecutionControl::renew(resources::PermitLease &lease) {
    const auto &ports = work_.execution_catalog.executor(lease.executor_index).ports;
    // Both port calls run under one callout: nothing between them reads the kernel.
    const ayni_status_t renewed = work_.call_out([&]() -> ayni_status_t {
        ayni_executor_nonce_v1 nonce{};
        const auto challenged = ports.challenge(ports.ctx, &lease.permit.epochs, &nonce);
        if (challenged != AYNI_OK) {
            return challenged;
        }
        if (nonce.control_domain_id != lease.permit.control_domain_id || nonce.nonce == 0) {
            return AYNI_INVALID_ARGUMENT;
        }
        const ayni_permit_renewal_v1 renewal{sizeof(renewal), 0, lease.permit.permit_id,
                                             nonce.nonce, lease.permit.epochs};
        return ports.renew(ports.ctx, &renewal);
    });
    lease.port_status = renewed;
    if (renewed != AYNI_OK && !permits_.request_stop(lease.call_id)) {
        return Advance::internal;
    }
    return Advance::applied;
}

RuntimeAdvance ExecutionControl::advance(uint64_t now_us, uint64_t durable_seq, RuntimeMode mode) {
    for (uint32_t scanned = 0; scanned < permits_.capacity(); ++scanned) {
        const uint32_t index = cursor_;
        cursor_ = (cursor_ + 1U) % permits_.capacity();
        const auto result = poll(index, now_us, durable_seq, mode);
        if (result != Advance::idle) {
            return result;
        }
    }
    return Advance::idle;
}

// Match independent readiness without consulting Provider queues. Evaluating
// authorization here is read-only; callbacks are confined to advance_control.
bool ExecutionControl::ready(uint64_t now_us, uint64_t durable_seq, RuntimeMode mode) const {
    for (uint32_t at = 0; at < permits_.capacity(); ++at) {
        const auto *lease = permits_.at(at);
        if (lease == nullptr) {
            continue;
        }
        if (lease->stopped) {
            if (stopped_releasable(*lease, durable_seq, mode)) {
                return true;
            }
            continue;
        }
        if (now_us >= lease->poll_after_us) {
            return true;
        }
        if (lease->phase == resources::PermitLease::Phase::stopping) {
            continue;
        }
        if (should_stop(*lease, now_us, durable_seq, mode)) {
            return true;
        }
    }
    return false;
}
} // namespace ayni::kernel
