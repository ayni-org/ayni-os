#include "kernel/service/execution/execution_control.h"

#include "authority/scope/scope.h"
#include "catalog/capability/business_encoding.h"
#include "foundation/deadline.h"

namespace ayni::kernel {
namespace {
ayni_string_v1 view(const catalog::CatalogId &id) {
    return {id.data(), id.size()};
}

// A challenge answered OK carries a usable nonce only in the permit's domain.
bool valid_nonce(const ayni_executor_nonce_v1 &nonce, uint64_t control_domain_id) {
    return nonce.control_domain_id == control_domain_id && nonce.nonce != 0;
}
} // namespace

// Projection rows share this one Scene generation. Control envelope fields are
// proven fixed at catalog close, but other Scene-limited fields can vary per
// item, and must not borrow authorization from only the first item (ADR-0084).
ayni_status_t ExecutionControl::authorize_input(
    const catalog::ExecutionBinding &binding, ayni_string_v1 capability, uint32_t version,
    ayni_bytes_v1 domain, const catalog::CallContract &contract, const work::CallInputView &input,
    const catalog::DecodedBusiness &proof, uint64_t durable_seq, uint32_t &scene) const {
    if (contract.input_format != catalog::call_value_input_format) {
        return work_.scenes.authorize(view(binding.scene), binding.scene_version, capability,
                                      version, input.arguments, durable_seq, scene);
    }
    const uint32_t count = catalog::projection_rows(proof.view(), input.value.view());
    if (count == 0U) {
        return AYNI_DENIED;
    }
    uint32_t selected = 0;
    for (uint32_t at = 0; at < count; ++at) {
        catalog::ProjectionRow row;
        auto status = catalog::project_row(proof.view(), input.value.view(), at, row);
        if (status != AYNI_OK) {
            return status;
        }
        std::array<uint8_t, AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX> actual{};
        uint32_t size = 0;
        status = authority::normalize_request(domain, row.view(), actual.data(),
                                              static_cast<uint32_t>(actual.size()), size);
        if (status != AYNI_OK) {
            return status;
        }
        status = work_.scenes.authorize(view(binding.scene), binding.scene_version, capability,
                                        version, {actual.data(), size}, durable_seq, selected);
        if (status != AYNI_OK) {
            return status;
        }
    }
    scene = selected;
    return AYNI_OK;
}

// Save the exact enabled Scene generation and measured prerequisites alongside
// the canonical call input. No permit or local nonce is issued during admission.
authority::GrantCheck ExecutionControl::prepare(
    const catalog::CallContract &contract, const catalog::Binding &binding, ayni_bytes_v1 domain,
    const work::CallInputView &input, const catalog::DecodedBusiness &proof, uint64_t now_us,
    uint64_t durable_seq, uint64_t task_id, std::optional<work::ExecutionFact> &out_fact) const {
    if ((contract.effects & catalog::call_effect_physical) == 0U) {
        return {AYNI_OK, AYNI_REASON_NONE};
    }
    uint32_t mapping = 0;
    uint32_t executor = 0;
    if (!work_.catalogs.execution.find_binding(view(binding.id), binding.version, mapping)) {
        return {AYNI_UNSUPPORTED, AYNI_REASON_INTENT_UNSUPPORTED};
    }
    const auto &spec = work_.catalogs.execution.binding(mapping);
    uint32_t scene = 0;
    const auto authorized =
        authorize_input(spec, view(binding.capability), binding.capability_version, domain,
                        contract, input, proof, durable_seq, scene);
    if (authorized != AYNI_OK) {
        return {authorized, AYNI_REASON_EXECUTION_SCENE};
    }
    uint32_t state = 0;
    if (!work_.scenes.find_state(view(spec.scene), state) ||
        !work_.catalogs.execution.find_executor(view(spec.executor), spec.executor_version,
                                                executor) ||
        work_.principals.binding() == nullptr) {
        return {AYNI_INTERNAL, AYNI_REASON_NONE};
    }
    const auto &ports = work_.catalogs.execution.executor(executor).ports;
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
                                                  fact.dependencies[at], task_id);
        if (status != AYNI_OK) {
            return {status, AYNI_REASON_EXECUTION_OBSERVATION};
        }
    }
    ayni_control_envelope_v1 envelope{};
    if (!work::valid_execution_metadata(fact) ||
        catalog::execution_envelope_for_value(fact.binding, proof.view(), input.value.view(),
                                              input.arguments, envelope) != AYNI_OK) {
        return {AYNI_INVALID_ARGUMENT, AYNI_REASON_GRANT_INPUT};
    }
    out_fact = fact;
    return {AYNI_OK, AYNI_REASON_NONE};
}

// Used immediately before dispatch and every local renewal. Scene close/reopen,
// a new observation version or a new boot cannot revive an accepted old action.
bool ExecutionControl::execution_current(uint32_t index) const {
    const auto &state = work_.calls.state(index);
    const auto &fact = *state.execution;
    const auto *owner = work_.principals.binding();
    return !state.content_erased && !work::execution_stopped(fact) &&
           fact.epochs.kernel_boot_epoch == work_.ids.boot_epoch() && owner != nullptr &&
           fact.epochs.authority_epoch == owner->binding_revision;
}

bool ExecutionControl::allowed(uint32_t index, uint64_t now_us, uint64_t durable_seq) const {
    if (!work_.calls.state(index).execution) {
        return true;
    }
    if (!execution_current(index)) {
        return false;
    }
    // prepare admitted every projection row against this immutable Scene/input in this boot.
    // Only its live generation and prerequisites can change (ADR-0103); a new boot was refused
    // above, and disable/re-enable cannot revive the old accepted generation.
    const auto &fact = *work_.calls.state(index).execution;
    uint32_t scene = 0, state = 0;
    return work_.scenes.current(view(fact.binding.scene), fact.binding.scene_version, durable_seq,
                                scene) == AYNI_OK &&
           work_.scenes.find_state(view(fact.binding.scene), state) &&
           work_.scenes.state(state).stamp.commit_seq == fact.scene_seq &&
           dependencies_current(index, now_us);
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
            observations_.require(definition.conditions[at].predicate, now_us, fresh,
                                  work_.calls.identity(index).task_id) != AYNI_OK) {
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

bool ExecutionControl::executor_of(const work::ExecutionFact &fact, uint32_t &out_index) const {
    return work_.catalogs.execution.find_executor(view(fact.binding.executor),
                                                  fact.binding.executor_version, out_index);
}

// One identity per request, from the sequence Provider attempts use, so a
// late answer can never name a newer request (C-ID-006).
template <typename Port>
RuntimeAdvance ExecutionControl::send(uint32_t executor, ExecutorLane lane,
                                      ayni_executor_request_t kind, uint64_t call_id,
                                      bool after_terminal, uint64_t now_us, Port port) {
    const auto &ports = work_.catalogs.execution.executor(executor).ports;
    ayni_attempt_ref_v1 identity{};
    if (!attempts_.next(work_.ids.boot_epoch(), identity)) {
        return Advance::attempt_exhausted;
    }
    const ExecutorRequest request{identity, kind, call_id,
                                  foundation::deadline(now_us, ports.poll_period_us),
                                  after_terminal};
    auto *target = executors_.open(executor, lane, request);
    if (target == nullptr) {
        return Advance::internal;
    }
    const auto status = work_.call_out([&] { return port(ports, request.identity, target); });
    if (status != AYNI_OK) {
        executors_.refuse(executor, lane, status);
    }
    return Advance::applied;
}

ExecutionControl::Reply ExecutionControl::take(uint32_t executor, ExecutorLane lane,
                                               uint64_t now_us, ExecutorRequest &out_request,
                                               ExecutorAnswer &out_answer) {
    const auto *request = executors_.request(executor, lane);
    if (request == nullptr) {
        return Reply::pending;
    }
    Reply reply = Reply::answered;
    if (!executors_.answer(executor, lane, out_answer)) {
        if (now_us < request->deadline_us) {
            return Reply::pending;
        }
        out_answer = {AYNI_RESULT_UNKNOWN, {}};
        reply = Reply::expired;
    }
    out_request = *request;
    executors_.close(executor, lane);
    return reply;
}

bool ExecutionControl::due(uint32_t executor, ExecutorLane lane, uint64_t now_us) const {
    const auto *request = executors_.request(executor, lane);
    ExecutorAnswer answer{};
    return request != nullptr &&
           (now_us >= request->deadline_us || executors_.answer(executor, lane, answer));
}

bool ExecutionControl::any_due(uint32_t executor, uint64_t now_us) const {
    return due(executor, ExecutorLane::stop, now_us) ||
           due(executor, ExecutorLane::status, now_us) ||
           due(executor, ExecutorLane::control, now_us);
}

// The saved DISPATCHING barrier is already durable. A challenge goes out
// first; once its answer arrives the caller has rechecked the gates, the fresh
// nonce anchors the executor's deadline and the full envelope is derived from
// authorized bytes. Nothing reaches a Provider before the lease is installed.
PermitIssue ExecutionControl::issue(uint32_t index, const resources::ResourceClaim &claim,
                                    const work::CallInputView &input,
                                    const catalog::DecodedBusiness &proof, uint64_t now_us) {
    const auto &saved = work_.calls.state(index).execution;
    if (!saved) {
        return PermitIssue::issued;
    }
    const auto &fact = *saved;
    uint32_t executor = 0;
    if (!executor_of(fact, executor)) {
        return PermitIssue::refused;
    }
    if (!permits_.available(executor)) {
        return PermitIssue::busy;
    }
    const auto &id = work_.calls.identity(index);
    const auto *request = executors_.request(executor, ExecutorLane::control);
    if (request == nullptr) {
        const auto sent = send(
            executor, ExecutorLane::control, AYNI_EXECUTOR_CHALLENGE, id.call_id, false, now_us,
            [&](const ayni_executor_v1 &ports, ayni_attempt_ref_v1 identity,
                ayni_executor_completion_t *target) {
                return ports.challenge(ports.ctx, identity, &fact.epochs, target);
            });
        return sent == Advance::applied             ? PermitIssue::requested
               : sent == Advance::attempt_exhausted ? PermitIssue::exhausted
                                                    : PermitIssue::defect;
    }
    if (request->call_id != id.call_id) {
        return PermitIssue::busy;
    }
    ExecutorRequest sent{};
    ExecutorAnswer answer{};
    const auto reply = take(executor, ExecutorLane::control, now_us, sent, answer);
    if (reply == Reply::pending) {
        return PermitIssue::waiting;
    }
    const auto &nonce = answer.output.nonce;
    if (answer.status != AYNI_OK || !valid_nonce(nonce, fact.control_domain)) {
        return PermitIssue::refused;
    }
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
    // The first status read waits one poll period, so a Provider that forwards
    // the permit to a remote gate has that long before a NOT_FOUND fences it.
    const uint64_t first_poll_us = foundation::deadline(
        now_us, work_.catalogs.execution.executor(executor).ports.poll_period_us);
    if (catalog::execution_envelope_for_value(fact.binding, proof.view(), input.value.view(),
                                              input.arguments, permit.envelope) != AYNI_OK ||
        !permits_.install(id.call_id, executor, claim, permit, first_poll_us)) {
        return PermitIssue::refused;
    }
    return PermitIssue::issued;
}

bool ExecutionControl::issue_ready(uint32_t index, uint64_t now_us) const {
    const auto &saved = work_.calls.state(index).execution;
    uint32_t executor = 0;
    if (!saved || !executor_of(*saved, executor)) {
        return true;
    }
    if (!permits_.available(executor)) {
        return false;
    }
    const auto *request = executors_.request(executor, ExecutorLane::control);
    return request == nullptr || (request->call_id == work_.calls.identity(index).call_id &&
                                  due(executor, ExecutorLane::control, now_us));
}

void ExecutionControl::abandon_issue(uint64_t call_id) {
    if (permits_.find(call_id) != nullptr) {
        return; // Its control lane carries renewals, which end with the lease.
    }
    for (uint32_t executor = 0; executor < permits_.capacity(); ++executor) {
        const auto *request = executors_.request(executor, ExecutorLane::control);
        if (request != nullptr && request->call_id == call_id) {
            executors_.close(executor, ExecutorLane::control);
        }
    }
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

bool ExecutionControl::accept_inline_stop(uint32_t index, const ayni_executor_status_v1 &status) {
    const auto &saved = work_.calls.state(index).execution;
    if (!saved || work::execution_stopped(*saved)) {
        return false;
    }
    uint32_t executor = 0;
    if (!executor_of(*saved, executor)) {
        return false;
    }
    const auto &ports = work_.catalogs.execution.executor(executor).ports;
    if ((ports.flags & AYNI_EXECUTOR_FLAG_INLINE_STOP_PROOF) == 0U ||
        status.control.phase != AYNI_CONTROL_STOPPED) {
        return false;
    }
    const auto call_id = work_.calls.identity(index).call_id;
    return permits_.observe(call_id, status) && stop_measured(call_id);
}

bool ExecutionControl::release_ephemeral_stopped(uint64_t call_id) {
    uint32_t call_index = 0;
    if (!work_.calls.find(call_id, call_index)) {
        return false;
    }
    const auto &state = work_.calls.state(call_index);
    const auto *lease = permits_.find(call_id);
    if (!state.ephemeral || !state.execution || !work::execution_stopped(*state.execution) ||
        lease == nullptr || !lease->stopped) {
        return false;
    }
    const uint32_t executor = lease->executor_index;
    if (!permits_.release_stopped(call_id)) {
        return false;
    }
    for (auto lane : {ExecutorLane::control, ExecutorLane::stop, ExecutorLane::status}) {
        executors_.close(executor, lane);
    }
    return true;
}

bool ExecutionControl::stop_measured(uint64_t call_id) const {
    const auto *lease = permits_.find(call_id);
    return lease != nullptr && lease->stopped.has_value();
}

bool ExecutionControl::holds_lease(uint64_t call_id) const {
    return permits_.find(call_id) != nullptr;
}

bool ExecutionControl::idle() const {
    return permits_.empty() && executors_.idle();
}

// A lease's poll waits while its status read is open; that read's own
// deadline, or its answer, is what wakes the owner then.
uint64_t ExecutionControl::next_deadline_us() const {
    uint64_t next = executors_.next_deadline_us();
    for (uint32_t at = 0; at < permits_.capacity(); ++at) {
        const auto *lease = permits_.at(at);
        if (lease != nullptr && !lease->stopped &&
            executors_.request(at, ExecutorLane::status) == nullptr &&
            lease->poll_after_us < next) {
            next = lease->poll_after_us;
        }
    }
    return next;
}

bool ExecutionControl::stop_unsent(const resources::PermitLease &lease) const {
    return lease.phase == resources::PermitLease::Phase::stopping &&
           lease.stop_delivery == resources::PermitLease::StopDelivery::pending &&
           executors_.request(lease.executor_index, ExecutorLane::stop) == nullptr;
}

// Durable work facts retain measured stop independently of Provider completion.
// Resource quarantine is lifted only by the independent consumer after this stamp persists,
// except for RAM-only EPHEMERAL calls whose stop fact never enters the WAL (ADR-0110).
RuntimeAdvance ExecutionControl::commit_stop(uint32_t index) {
    const auto *lease = permits_.find(work_.calls.identity(index).call_id);
    if (lease == nullptr || !lease->stopped) {
        return Advance::internal;
    }
    const auto &fact = *lease->stopped;
    const foundation::TransitionStamp stamp = work_.calls.state(index).ephemeral
                                                  ? foundation::TransitionStamp{0, 0}
                                                  : work_.transitions.next();
    return work_.calls.confirm_execution(index, fact.phase, fact.cause, fact.target,
                                         {work_.ids.boot_epoch(), stamp}) == AYNI_OK
               ? Advance::applied
               : Advance::internal;
}

// Lease slots outlive work-model reload. Inhibition never trusts a partially
// restored CallTable index or waits for a new writer to finish recovery. A
// stop goes out on its own lane, whatever the executor's other lanes await,
// and one unanswered in time is taken here too, so it goes out again.
RuntimeAdvance ExecutionControl::advance_urgent(uint64_t now_us, uint64_t durable_seq,
                                                RuntimeMode mode) {
    for (uint32_t at = 0; at < permits_.capacity(); ++at) {
        const auto *lease = permits_.at(at);
        if (lease == nullptr || lease->stopped) {
            continue;
        }
        const bool urgent = lease->phase == resources::PermitLease::Phase::issued
                                ? should_stop(*lease, now_us, durable_seq, mode)
                                : stop_unsent(*lease) || due(at, ExecutorLane::stop, now_us);
        if (urgent) {
            return poll(at, now_us, durable_seq, mode);
        }
    }
    return Advance::idle;
}

// Independent ports never mutate persistent facts. In particular, journal
// backpressure cannot delay disabling output or maintaining actuator quarantine.
// One item sends one request or applies one answer, never both for the same
// request; the gate allowed is evaluated fresh in every item.
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
            // Later answers for this permit are refused.
            for (auto lane : {ExecutorLane::control, ExecutorLane::stop, ExecutorLane::status}) {
                executors_.close(index, lane);
            }
            // The task table reads the lease (CallRuntime::runtime_retains)
            // without a transition, also one restored without a call slot.
            work_.forget_due_work();
            transport_.retire(transport_.context, call_id, now_us);
            return Advance::applied;
        }
        // The measured stop needs nothing more from the executor.
        for (auto lane : {ExecutorLane::control, ExecutorLane::stop, ExecutorLane::status}) {
            if (due(index, lane, now_us)) {
                executors_.close(index, lane);
                return Advance::applied;
            }
        }
        return Advance::idle;
    }
    const bool allowed = !should_stop(*lease, now_us, durable_seq, mode);
    if (!allowed && !permits_.request_stop(lease->call_id)) {
        return Advance::internal;
    }
    if (stop_unsent(*lease)) {
        // A not-installed permit remains quarantined until terminal + authoritative absence.
        // An unanswered stop is sent again, a refused one leaves the gate to
        // expire; no answer is physical stop evidence.
        lease->stop_delivery = resources::PermitLease::StopDelivery::sent;
        lease->poll_after_us = now_us;
        const uint64_t permit_id = lease->permit.permit_id;
        return send(index, ExecutorLane::stop, AYNI_EXECUTOR_STOP, lease->call_id, false, now_us,
                    [&](const ayni_executor_v1 &ports, ayni_attempt_ref_v1 identity,
                        ayni_executor_completion_t *target) {
                        return ports.stop(ports.ctx, identity, permit_id, target);
                    });
    }
    for (auto lane : {ExecutorLane::stop, ExecutorLane::status, ExecutorLane::control}) {
        if (due(index, lane, now_us)) {
            return apply(*lease, lane, now_us, allowed);
        }
    }
    if (now_us < lease->poll_after_us ||
        executors_.request(index, ExecutorLane::status) != nullptr) {
        return Advance::idle;
    }
    lease->poll_after_us =
        foundation::deadline(now_us, work_.catalogs.execution.executor(index).ports.poll_period_us);
    const uint64_t permit_id = lease->permit.permit_id;
    // Absence is authoritative only in a read sent after the Provider's terminal.
    return send(index, ExecutorLane::status, AYNI_EXECUTOR_STATUS, lease->call_id,
                lease->terminal_seen, now_us,
                [&](const ayni_executor_v1 &ports, ayni_attempt_ref_v1 identity,
                    ayni_executor_completion_t *target) {
                    return ports.status(ports.ctx, identity, permit_id, target);
                });
}

// A renewal is a chain: an ACTIVE status sends a challenge, and the
// challenge's answer sends the renewal only if allowed still holds then. So
// the nonce a renewal echoes was issued before Core last found the gate open.
ExecutionControl::Advance ExecutionControl::apply(resources::PermitLease &lease, ExecutorLane lane,
                                                  uint64_t now_us, bool allowed) {
    const uint32_t index = lease.executor_index;
    ExecutorRequest request{};
    ExecutorAnswer answer{};
    const auto reply = take(index, lane, now_us, request, answer);
    if (reply == Reply::pending) {
        return Advance::idle;
    }
    lease.port_status = answer.status;
    if (lane == ExecutorLane::stop) {
        if (reply == Reply::expired) {
            lease.stop_delivery = resources::PermitLease::StopDelivery::pending;
        }
        return Advance::applied;
    }
    const bool renewing = allowed && lease.phase == resources::PermitLease::Phase::issued &&
                          lease.permit.form == AYNI_FORM_SESSION;
    if (lane == ExecutorLane::status) {
        if (answer.status == AYNI_NOT_FOUND && request.after_terminal) {
            return permits_.not_started(lease.call_id) ? Advance::applied : Advance::internal;
        }
        if (answer.status == AYNI_NOT_FOUND && lease.terminal_seen) {
            lease.poll_after_us = now_us; // Sent before the terminal: ask again.
        }
        if (answer.status == AYNI_OK && !permits_.observe(lease.call_id, answer.output.status)) {
            lease.port_status = AYNI_INVALID_ARGUMENT;
        }
        if (lease.port_status != AYNI_OK) {
            return permits_.request_stop(lease.call_id) ? Advance::applied : Advance::internal;
        }
        if (!renewing || answer.output.status.control.phase != AYNI_CONTROL_ACTIVE ||
            executors_.request(index, ExecutorLane::control) != nullptr) {
            return Advance::applied;
        }
        // Renewal uses the original complete generations and a new executor-local nonce.
        const auto epochs = lease.permit.epochs;
        return send(index, ExecutorLane::control, AYNI_EXECUTOR_CHALLENGE, lease.call_id, false,
                    now_us,
                    [&](const ayni_executor_v1 &ports, ayni_attempt_ref_v1 identity,
                        ayni_executor_completion_t *target) {
                        return ports.challenge(ports.ctx, identity, &epochs, target);
                    });
    }
    if (request.kind == AYNI_EXECUTOR_CHALLENGE && answer.status == AYNI_OK &&
        !valid_nonce(answer.output.nonce, lease.permit.control_domain_id)) {
        lease.port_status = AYNI_INVALID_ARGUMENT;
    }
    if (lease.port_status != AYNI_OK) {
        return permits_.request_stop(lease.call_id) ? Advance::applied : Advance::internal;
    }
    if (request.kind != AYNI_EXECUTOR_CHALLENGE || !renewing) {
        return Advance::applied;
    }
    const ayni_permit_renewal_v1 renewal{sizeof(renewal), 0, lease.permit.permit_id,
                                         answer.output.nonce.nonce, lease.permit.epochs};
    return send(index, ExecutorLane::control, AYNI_EXECUTOR_RENEW, lease.call_id, false, now_us,
                [&](const ayni_executor_v1 &ports, ayni_attempt_ref_v1 identity,
                    ayni_executor_completion_t *target) {
                    return ports.renew(ports.ctx, identity, &renewal, target);
                });
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
    if (!work_.calls.find(lease.call_id, index)) {
        return false;
    }
    const auto &state = work_.calls.state(index);
    if (!state.execution || !work::execution_stopped(*state.execution)) {
        return false;
    }
    if (state.ephemeral) {
        return true; // Confirmed physical stop lifts RAM quarantine without durable_seq (ADR-0110).
    }
    return mode != Mode::fenced && state.latest.transition.commit_seq <= durable_seq;
}

// All execution gates share the same decision for renewal and immediate stopping.
bool ExecutionControl::should_stop(const resources::PermitLease &lease, uint64_t now_us,
                                   uint64_t durable_seq, Mode mode) const {
    if (mode != Mode::running || lease.terminal_seen) {
        return true;
    }
    bool expired = false;
    uint32_t index = 0;
    if (!transport_.holds(transport_.context, lease.call_id, now_us, expired) ||
        !work_.calls.find(lease.call_id, index)) {
        return true;
    }
    const auto &state = work_.calls.state(index);
    if (state.reflex) {
        return expired || state.cancel_cause != work::CallCancelCause::none ||
               work_.check_ephemeral_call_authority(index, now_us, durable_seq).status != AYNI_OK ||
               !allowed(index, now_us, durable_seq) || !resources_.executable(lease.claim);
    }
    uint32_t task_index = 0;
    if (!work_.tasks.find(work_.calls.identity(index).task_id, task_index)) {
        return true;
    }
    const auto &task = work_.tasks.at(task_index);
    return expired || task.call.end != work::TaskCall::End::none ||
           !work::TaskTable::is_live(task.lifecycle) || task.lifecycle == AYNI_TASK_PAUSED ||
           state.cancel_cause != work::CallCancelCause::none ||
           work_.check_task_authority(task_index, now_us, durable_seq).status != AYNI_OK ||
           work_.check_ephemeral_call_authority(index, now_us, durable_seq).status != AYNI_OK ||
           !allowed(index, now_us, durable_seq) || !resources_.executable(lease.claim);
}

// Capture transient user/group pauses before the owner can accept a resume.
// The lease owner makes this stop irreversible; this call never enters an executor port.
bool ExecutionControl::latch_closed_gates(uint64_t now_us, uint64_t durable_seq, RuntimeMode mode) {
    for (uint32_t index = 0; index < permits_.capacity(); ++index) {
        const auto *lease = permits_.at(index);
        if (lease != nullptr && !lease->stopped && should_stop(*lease, now_us, durable_seq, mode) &&
            !permits_.request_stop(lease->call_id)) {
            return false;
        }
    }
    return true;
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
// authorization here is read-only; port callbacks are confined to advance
// and advance_urgent. An unanswered request is ready only at its deadline.
bool ExecutionControl::ready(uint64_t now_us, uint64_t durable_seq, RuntimeMode mode) const {
    for (uint32_t at = 0; at < permits_.capacity(); ++at) {
        const auto *lease = permits_.at(at);
        if (lease == nullptr) {
            continue;
        }
        if (any_due(at, now_us)) {
            return true;
        }
        if (lease->stopped) {
            if (stopped_releasable(*lease, durable_seq, mode)) {
                return true;
            }
            continue;
        }
        if (now_us >= lease->poll_after_us &&
            executors_.request(at, ExecutorLane::status) == nullptr) {
            return true;
        }
        if (lease->phase == resources::PermitLease::Phase::stopping) {
            if (stop_unsent(*lease)) {
                return true;
            }
            continue;
        }
        if (should_stop(*lease, now_us, durable_seq, mode)) {
            return true;
        }
    }
    return false;
}
} // namespace ayni::kernel
