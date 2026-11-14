#include "foundation/workspace.h"
#include "kernel/service/call/call_runtime.h"
#include "kernel/service/memory/memory_permissions.h"
#include "work/call_propagation.h"

#include "foundation/deadline.h"

namespace ayni::kernel {
namespace {
ayni_string_v1 view(const catalog::CatalogId &id) {
    return {id.data(), id.size()};
}

// Fields borrow already canonical request bytes through this callback only.
// Values are never separately supplied by a model or authorization projection.
struct ConcreteArguments {
    std::array<ayni_scope_field_v1, AYNI_AUTHORITY_FIELD_COUNT_MAX> fields{};
    std::array<catalog::DomainField, AYNI_AUTHORITY_FIELD_COUNT_MAX> decoded{};
    uint32_t count = 0;
    bool read(ayni_bytes_v1 bytes) {
        catalog::DomainReader reader(bytes);
        while (count < fields.size() && reader.next(decoded[count])) {
            const auto &field = decoded[count];
            auto &out = fields[count];
            out.struct_size = sizeof(out);
            out.field_id = field.id;
            out.allowed.struct_size = sizeof(out.allowed);
            out.allowed.kind = field.values.kind;
            out.allowed.unit = field.values.unit;
            if (field.values.kind == AYNI_AUTHORITY_I64) {
                out.allowed.values.integer = {field.values.minimum, field.values.maximum};
            } else {
                out.allowed.values.tokens = {field.values.tokens.data(), field.values.token_count,
                                             0};
            }
            ++count;
        }
        return reader.ok();
    }
    ayni_scope_v1 view() const { return {sizeof(ayni_scope_v1), count, fields.data()}; }
};
} // namespace

// Restore identities without restoring a submission claim. An old possible
// dispatch becomes UNKNOWN; only a never-dispatched request can join a new queue.
CallRuntime::Advance CallRuntime::restore_wait(uint32_t index, uint32_t task_index, uint64_t now_us,
                                               uint64_t durable_seq, Mode mode) {
    if (mode == Mode::fenced) {
        return Advance::idle;
    }
    const auto &id = work_.calls.identity(index);
    const auto *action = work_.calls.action(index);
    if (action != nullptr && (action->phase == work::ActionPhase::dispatching ||
                              action->phase == work::ActionPhase::in_progress)) {
        const auto stamp = work_.transitions.next();
        if (work_.calls.finish(work_.workspace, index,
                               {AYNI_CALL_ABANDONED,
                                AYNI_RESULT_UNKNOWN,
                                AYNI_REASON_NONE,
                                work::ActionEffect::unknown,
                                {}},
                               {work_.ids.boot_epoch(), stamp}) != AYNI_OK ||
            !work::end_task_call(work_.calls, work_.tasks, index, task_index, stamp)) {
            return Advance::internal;
        }
        return Advance::applied;
    }
    // C-CALL-008 / C-PROD-030: a restored possible dispatch must become
    // UNKNOWN above even after its source or authority was invalidated. Only
    // known unsubmitted work may take the local non-delivery cancellation path.
    const auto &task = work_.tasks.at(task_index);
    if (!model_context_current(task_index, id.model_context, now_us, durable_seq)) {
        const auto result = cancel_local(index, task_index);
        if (result == Advance::applied) {
            work_.calls.discard_unusable_result(index);
        }
        return result;
    }
    if (!task_content_current({work_, now_us, durable_seq}, task_index) ||
        task.call.end != work::TaskCall::End::none || !work::TaskTable::is_live(task.lifecycle) ||
        (work_.calls.state(index).attachment == work::CallAttachment::background &&
         task.lifecycle == AYNI_TASK_PAUSED)) {
        return cancel_local(index, task_index);
    }
    // Expired waits persist through shutdown. User pause is independent and
    // also prevents a resumed call from joining the queue until released.
    // Only never-dispatched calls reach this gate (ADR-0047).
    if (work::TaskTable::is_held(task) || task.lifecycle == AYNI_TASK_PAUSED) {
        return Advance::idle;
    }
    catalog::BindingLinks links{};
    const bool compatible = compatible_call(index, links);
    if (!compatible) {
        slots_[index].active = Active{id.call_id, Stage::blocked, {}, std::nullopt};
        work_.tasks.hold_restored(task_index, AYNI_UNSUPPORTED_VERSION,
                                  AYNI_REASON_INTENT_UNSUPPORTED);
        work_.forget_due_work(); // A recovery hold, outside any transition.
        return Advance::applied;
    }
    if (!resources_.has_room()) {
        return Advance::idle;
    }
    if (resources_.enqueue(
            resource_request(id.call_id, id.owner_instance_id, links, work_.calls.contract(index),
                             work_.goals.at(work_.tasks.at(task_index).goal_index).work_profile),
            now_us) != AYNI_OK) {
        return Advance::internal;
    }
    slots_[index].active = Active{id.call_id, Stage::queued, {}, std::nullopt};
    return Advance::applied;
}

// Original immutable route/domain/contract must still exist; catalog indices
// are freshly resolved and never copied into durable Call state.
bool CallRuntime::compatible_call(uint32_t index, catalog::BindingLinks &links) const {
    const auto &id = work_.calls.identity(index);
    uint32_t binding = 0;
    return catalogs_.bindings.find(view(id.binding_id), id.binding_version, binding) &&
           catalog::BindingRegistry::same_selection(id.binding(), catalogs_.bindings.at(binding)) &&
           catalog::BindingRegistry::resolve(catalogs_.bindings.at(binding), catalogs_.capabilities,
                                             catalogs_.call_capabilities, catalogs_.providers,
                                             catalogs_.resources, links) == AYNI_OK &&
           catalog::same_call_contract(work_.calls.contract(index), links.contract) &&
           catalogs_.capabilities.matches_domain(links.capability, work_.calls.domain(index));
}

// Publish cancellation intent before contacting a Provider. A refused cancel
// leaves its original attempt live and never synthesizes a terminal receipt.
CallRuntime::Advance CallRuntime::stop(uint32_t index, uint32_t task_index, Mode mode,
                                       work::CallCancelCause cause) {
    auto &active = *slots_[index].active;
    if (work_.calls.state(index).subscription &&
        work_.calls.state(index).cancel_cause == work::CallCancelCause::none) {
        const auto &task = work_.tasks.at(task_index);
        const bool preserve = work::TaskTable::is_live(task.lifecycle) &&
                              task.call.end == work::TaskCall::End::none &&
                              cause != work::CallCancelCause::deadline &&
                              !work_.calls.state(index).content_erased;
        const auto desired =
            preserve ? work::SubscriptionDesired::open : work::SubscriptionDesired::closed;
        close_subscription_gate(index);
        return work_.calls.stop_subscription(index, cause, desired,
                                             {work_.ids.boot_epoch(), work_.transitions.next()}) ==
                       AYNI_OK
                   ? Advance::applied
                   : Advance::internal;
    }
    if (!submitted(active.stage)) {
        return mode == Mode::fenced ? Advance::idle : cancel_local(index, task_index);
    }
    const auto &state = work_.calls.state(index);
    if (state.cancel_cause == work::CallCancelCause::none && mode != Mode::fenced) {
        return work_.calls.request_cancel(
                   index, cause, {work_.ids.boot_epoch(), work_.transitions.next()}) == AYNI_OK
                   ? Advance::applied
                   : Advance::internal;
    }
    return cancel_transport(*active.delivery);
}

// The durable DISPATCHING fact precedes this sole callback. Recheck authority
// with fresh time and the actual retained arguments, then claim exactly once.
CallRuntime::Advance CallRuntime::dispatch(uint32_t index, uint32_t task_index, uint64_t now_us,
                                           uint64_t durable_seq) {
    const auto &id = work_.calls.identity(index);
    const auto &contract = work_.calls.contract(index);
    uint32_t binding_index = 0;
    uint32_t provider_index = 0;
    if (!catalogs_.bindings.find(view(id.binding_id), id.binding_version, binding_index) ||
        !catalogs_.providers.find(view(id.provider_id), id.provider_version, provider_index)) {
        return Advance::internal;
    }
    auto input_lease = work_.workspace.make<work::CallInputView>();
    auto proof_lease = work_.workspace.make<catalog::DecodedBusiness>();
    if (!input_lease || !proof_lease) {
        return Advance::internal;
    }
    auto &input = *input_lease;
    const auto *proof = &*proof_lease;
    const auto decoded = catalog::decode_business(work_.workspace, work_.calls.domain(index),
                                                  contract.business_contract, *proof_lease);
    if (decoded != AYNI_OK) {
        return Advance::internal;
    }
    if (work::read_call_input_with_proof(work_.workspace, *proof, work_.calls.domain(index),
                                         work_.calls.input(index), contract, input) != AYNI_OK) {
        return Advance::internal;
    }
    const auto authority =
        check_input(task_index, catalogs_.bindings.at(binding_index), work_.calls.domain(index),
                    input, contract, *proof, host_.monotonic_now_us(host_.ctx), durable_seq,
                    work_.calls.identity(index).call_id);
    if (authority.status != AYNI_OK) {
        return authority.status == AYNI_BUSY ? Advance::idle : cancel_local(index, task_index);
    }
    if (!resources_.executable(slots_[index].active->claim) ||
        !control_.allowed_with_proof(index, now_us, durable_seq, *proof, input)) {
        return cancel_local(index, task_index);
    }
    // The permit's challenge is answered in a later item, so this recheck
    // runs again before the nonce it brings is used (ADR-0052).
    switch (control_.issue(index, slots_[index].active->claim, input.arguments, now_us)) {
    case PermitIssue::issued:
        break;
    case PermitIssue::requested:
        return Advance::applied;
    case PermitIssue::waiting:
    case PermitIssue::busy:
        return Advance::idle;
    case PermitIssue::refused:
        return cancel_local(index, task_index);
    case PermitIssue::exhausted:
        return Advance::attempt_exhausted;
    case PermitIssue::defect:
        return Advance::internal;
    }
    if (!task_export_current({work_, host_.monotonic_now_us(host_.ctx), durable_seq}, task_index,
                             catalogs_.bindings.at(binding_index))) {
        return cancel_local(index, task_index);
    }
    if (!model_context_current(task_index, id.model_context, host_.monotonic_now_us(host_.ctx),
                               durable_seq)) {
        const auto result = cancel_local(index, task_index);
        if (result == Advance::applied) {
            work_.calls.discard_unusable_result(index);
        }
        return result;
    }
    if (!work_.calls.claim_dispatch(index, work_.ids.boot_epoch(), durable_seq)) {
        return Advance::internal;
    }
    return submit_provider(index, task_index, provider_index, input, now_us);
}

// Only this already-claimed original attempt reaches Provider code. Typed fields
// and the permit borrow are valid during submit; the completion lease survives it.
CallRuntime::Advance CallRuntime::submit_provider(uint32_t index, uint32_t task_index,
                                                  uint32_t provider_index,
                                                  const work::CallInputView &input,
                                                  uint64_t now_us) {
    const auto &id = work_.calls.identity(index);
    const auto &contract = work_.calls.contract(index);
    ConcreteArguments arguments;
    if (!arguments.read(input.arguments)) {
        return Advance::internal;
    }
    auto &active = *slots_[index].active;
    auto &delivery = *active.delivery;
    delivery.deadline_us =
        foundation::deadline(now_us, uint64_t{contract.timeout_ms} * microseconds_per_millisecond);
    active.stage = Stage::submitted;
    const auto *permit = control_.permit(id.call_id);
    ayni_subscription_open_v1 subscription{};
    if (contract.subscription && !prepare_subscription_delivery(index, now_us, subscription)) {
        return Advance::internal;
    }
    const ayni_provider_request_v1 request{
        sizeof(request),
        id.capability_version,
        delivery.reservation.attempt,
        id.call_id,
        contract.subscription ? work_.calls.state(index).subscription->generation : id.operation_id,
        delivery.deadline_us,
        view(id.capability_id),
        view(id.endpoint),
        arguments.view(),
        {},
        permit,
        contract.subscription ? &subscription : nullptr,
        input.value.count == 0U ? ayni_value_v1{} : input.value.view()};
    const auto &provider = catalogs_.providers.at(provider_index);
    delivery.provider = &provider;
    const auto submitted = work_.call_out([&] {
        return provider.submit(provider.context, &request,
                               mailbox_.completion(delivery.reservation));
    });
    if (submitted == AYNI_SUBMIT_REJECTED_NOT_RETAINED && permit != nullptr) {
        // Publish into the reserved reliable lane; absence still needs exact local gate evidence.
        const ProviderMessage rejected{
            ProviderMessageKind::failed, 0, AYNI_NOT_READY, AYNI_REASON_NONE, {},
            AYNI_CALL_EFFECT_NONE,       0};
        const auto posted = mailbox_.post(delivery.reservation, rejected);
        return posted == AYNI_OK || posted == AYNI_INVALID_STATE ? Advance::applied
                                                                 : Advance::internal;
    }
    if (submitted == AYNI_SUBMIT_REJECTED_NOT_RETAINED && mailbox_.withdraw(delivery.reservation)) {
        const auto stamp = work_.transitions.next();
        if (work_.calls.rejected_not_retained(index, {work_.ids.boot_epoch(), stamp}) != AYNI_OK ||
            !work::end_task_call(work_.calls, work_.tasks, index, task_index, stamp)) {
            return Advance::internal;
        }
        release(index, now_us);
    } else if (submitted != AYNI_SUBMIT_ACCEPTED && submitted != AYNI_SUBMIT_ACCEPT_UNKNOWN &&
               submitted != AYNI_SUBMIT_REJECTED_NOT_RETAINED) {
        return Advance::internal;
    }
    // A contradictory NOT_RETAINED cannot discard an inline posted receipt;
    // its reliable evidence still owns the slot and is consumed normally.
    return Advance::applied;
}
} // namespace ayni::kernel
