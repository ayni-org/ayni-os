#include "work/call/call_table.h"
#include "foundation/workspace.h"

#include "catalog/capability/value_encoding.h"
#include "foundation/kernel_id.h"

#include <cstring>
#include <new>

namespace ayni::work {
ActionEffect CallTable::effect(uint32_t index) const {
    const auto *fact = action(index);
    return fact == nullptr ? ActionEffect::none : fact->effect;
}
catalog::Binding CallIdentity::binding() const {
    return {binding_id,  binding_version,  capability_id, capability_version,
            provider_id, provider_version, endpoint,      destination};
}

// Call outcome and effect knowledge are independent. A Provider that has lost
// tracking must say ABANDONED, never turn uncertainty into a known failure.
bool CallTable::valid_result(const CallResult &result) {
    switch (result.outcome) {
    case AYNI_CALL_COMPLETED:
        return result.status == AYNI_OK && result.reason == AYNI_REASON_NONE &&
               result.effect == ActionEffect::complete;
    case AYNI_CALL_FAILED:
        return result.status < AYNI_OK && result.status != AYNI_RESULT_UNKNOWN;
    case AYNI_CALL_CANCELLED:
        return result.status == AYNI_OK && result.reason == AYNI_REASON_NONE;
    case AYNI_CALL_ABANDONED:
        return result.status == AYNI_RESULT_UNKNOWN && result.effect == ActionEffect::unknown &&
               result.text.size == 0U;
    default:
        return false;
    }
}

// A tool-call list is the whole of a TOOL_CALLS completion's body, never
// beside text, and is bounded like text by the contract's result limit.
ayni_status_t CallTable::check_body(ayni::foundation::Workspace &workspace,
                                    const catalog::CallContract &contract, ayni_string_v1 body,
                                    CallResultKind kind) {
    if (kind == CallResultKind::unavailable) {
        return body.size == 0U ? AYNI_OK : AYNI_INVALID_ARGUMENT;
    }
    if (kind == CallResultKind::text) {
        if (contract.result_format == catalog::call_value_result_format && body.size != 0U) {
            return AYNI_INVALID_ARGUMENT;
        }
        return check_call_text(body, contract.result_text_limit);
    }
    if (kind == CallResultKind::value) {
        if (contract.result_format != catalog::call_value_result_format ||
            body.size > contract.result_text_limit) {
            return AYNI_INVALID_ARGUMENT;
        }
        auto value_lease = workspace.make<catalog::DecodedValue>();
        if (!value_lease) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        auto &value = *value_lease;
        const auto decoded = catalog::decode_value(
            workspace, {reinterpret_cast<const uint8_t *>(body.data), body.size}, value);
        return decoded == AYNI_OK && catalog::canonical_value(value.view()) ? AYNI_OK
                                                                            : AYNI_INVALID_ARGUMENT;
    }
    if (kind != CallResultKind::tool_calls ||
        contract.result_format != catalog::call_tool_calls_result_format) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (body.size > contract.result_text_limit) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    return tool_call_count(workspace, {reinterpret_cast<const uint8_t *>(body.data), body.size}) !=
                   0U
               ? AYNI_OK
               : AYNI_INVALID_ARGUMENT;
}

ayni_status_t CallTable::check_result(ayni::foundation::Workspace &workspace, uint32_t index,
                                      const CallResult &result) const {
    if (!valid_result(result) || !valid_model_usage(result.model_usage) ||
        (result.model_usage.kind != AYNI_MODEL_USAGE_NONE &&
         slots_[index].entry->contract.category != catalog::CallClass::model) ||
        result.kind == CallResultKind::unavailable ||
        (result.kind == CallResultKind::tool_calls && result.outcome != AYNI_CALL_COMPLETED)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &contract = slots_[index].entry->contract;
    if (contract.result_format == catalog::call_value_result_format &&
        result.outcome == AYNI_CALL_COMPLETED && result.kind != CallResultKind::value) {
        return AYNI_INVALID_ARGUMENT;
    }
    return check_body(workspace, contract, result.text, result.kind);
}

// Stale evidence cannot yield a usable completed step. Keep measured side effects,
// but only after the original Provider shape/text have passed the shared validator.
CallResult CallTable::usable_result(const CallResult &validated, ResultDependencies dependencies) {
    auto result = validated;
    if (dependencies == ResultDependencies::stale && validated.outcome == AYNI_CALL_COMPLETED) {
        result.outcome = AYNI_CALL_FAILED;
        result.status = AYNI_STALE;
        result.reason = AYNI_REASON_EXECUTION_OBSERVATION;
        result.text = {};
        result.kind = CallResultKind::text;
    }
    return result;
}

namespace {
// The ledger validates effect enums and claimed-attempt transitions exactly
// once. All later body/state writes are infallible after this succeeds.
ayni_status_t finish_action(ActionRecord &action, const CallResult &result, ActionStamp stamp) {
    switch (result.outcome) {
    case AYNI_CALL_COMPLETED:
        return action.complete(stamp);
    case AYNI_CALL_FAILED:
        return action.fail(result.effect, stamp);
    case AYNI_CALL_CANCELLED:
        return action.cancelled(result.effect, stamp);
    case AYNI_CALL_ABANDONED:
        return action.abandon(stamp);
    default:
        return AYNI_INVALID_ARGUMENT;
    }
}
} // namespace

std::size_t CallTable::storage_size(Limits limits) {
    return static_cast<std::size_t>(limits.capacity) *
           (sizeof(Slot) + limits.domain_bytes + limits.input_bytes + limits.result_bytes +
            limits.business_bytes);
}

CallTable::CallTable(void *storage, Limits limits)
    : slots_(static_cast<Slot *>(storage)),
      bodies_(limits.capacity == 0U ? nullptr
                                    : reinterpret_cast<uint8_t *>(slots_ + limits.capacity)),
      limits_(limits) {
    static_assert(alignof(Slot) <= storage_alignment, "caller arena aligns stable call slots");
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        new (slots_ + at) Slot{};
    }
}

uint8_t *CallTable::bytes(uint32_t index) const {
    const auto stride = static_cast<std::size_t>(limits_.domain_bytes) + limits_.input_bytes +
                        limits_.result_bytes + limits_.business_bytes;
    return bodies_ + index * stride;
}

// The coordinator proves foreign-key ownership. This owner verifies intrinsic
// identity/stamp shape before reserving a stable slot or touching its byte pool.
bool CallTable::valid_identity(const CallIdentity &id, ActionStamp stamp) {
    const bool source = id.authorization.grant_id == 0U
                            ? id.authorization.revision == 0U
                            : foundation::is_kernel_id(id.authorization.grant_id) &&
                                  id.authorization.revision != 0U &&
                                  id.authorization.grant_id < id.call_id;
    bool disclosure = id.model_context.boot_epoch == stamp.boot_epoch;
    if (!id.model_context.boot_epoch) {
        disclosure = true;
        for (const auto word : id.model_context.visible) {
            if (word) {
                disclosure = false;
                break;
            }
        }
    }
    return source && disclosure && id.model_context.valid() &&
           foundation::is_kernel_id(id.call_id) && foundation::is_kernel_id(id.operation_id) &&
           foundation::is_kernel_id(id.task_id) && foundation::is_kernel_id(id.owner_instance_id) &&
           id.call_id != id.operation_id && id.call_id != id.task_id &&
           id.operation_id != id.task_id &&
           foundation::id_boot_epoch(id.call_id) == stamp.boot_epoch &&
           foundation::id_boot_epoch(id.operation_id) == stamp.boot_epoch &&
           foundation::id_boot_epoch(id.task_id) <= stamp.boot_epoch &&
           foundation::id_boot_epoch(id.owner_instance_id) <= stamp.boot_epoch &&
           stamp.transition.transition_id != 0U && stamp.transition.commit_seq != 0U &&
           id.step_key.size() != 0U && id.binding_id.size() != 0U && id.binding_version != 0U &&
           id.capability_id.size() != 0U && id.capability_version != 0U &&
           id.provider_id.size() != 0U && id.provider_version != 0U && id.endpoint.size() != 0U;
}

bool CallTable::advances(const Entry &entry, ActionStamp stamp) {
    const auto &latest = entry.state.latest;
    return stamp.boot_epoch >= latest.boot_epoch &&
           stamp.transition.transition_id > latest.transition.transition_id &&
           stamp.transition.commit_seq > latest.transition.commit_seq;
}

bool CallTable::pending(const Entry &entry) {
    return entry.state.outcome == 0U;
}

// Admission already validated the canonical input against this exact domain
// and contract. Capacity/duplicate refusal changes neither metadata nor bytes.
bool CallTable::valid_mode(const CallIdentity &id, const catalog::CallContract &contract) {
    return id.mode == AYNI_CALL_MODE_FOREGROUND ||
           (id.mode == AYNI_CALL_MODE_MANAGED && contract.category == catalog::CallClass::tool &&
            (contract.form == catalog::CallForm::action ||
             contract.form == catalog::CallForm::subscription));
}

ayni_status_t CallTable::insert(const CallIdentity &id, const catalog::CallContract &contract,
                                ayni_bytes_v1 domain_body, ayni_bytes_v1 input_body,
                                ActionStamp stamp, uint32_t &index) {
    if (!valid_identity(id, stamp) || !valid_mode(id, contract) || domain_body.data == nullptr ||
        input_body.data == nullptr || domain_body.size == 0U ||
        input_body.size < catalog::value_encoding_header_bytes ||
        ((contract.effects & catalog::call_effect_egress) != 0U) != (id.destination.size() != 0U)) {
        return AYNI_INVALID_ARGUMENT;
    }
    return place_new(id, contract, domain_body, input_body, stamp, index);
}
// Allocation and immutable identity uniqueness have one owner for admission and snapshot restore.
ayni_status_t CallTable::place_new(const CallIdentity &id, const catalog::CallContract &contract,
                                   ayni_bytes_v1 domain_body, ayni_bytes_v1 input_body,
                                   ActionStamp stamp, uint32_t &index) {
    uint32_t duplicate = 0;
    if (find(id.call_id, duplicate) ||
        find_step(id.task_id, {id.step_key.data(), id.step_key.size()}, duplicate)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (!has_room() || domain_body.size > limits_.domain_bytes ||
        input_body.size > limits_.input_bytes ||
        contract.result_text_limit > limits_.result_bytes ||
        contract.business_contract.size > limits_.business_bytes) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        if (slots_[at].entry) {
            if (slots_[at].entry->identity.operation_id == id.operation_id) {
                return AYNI_INVALID_ARGUMENT;
            }
        }
    }
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        auto &slot = slots_[at];
        if (slot.entry) {
            continue;
        }
        slot.entry.emplace();
        auto &entry = *slot.entry;
        entry.identity = id;
        entry.contract = contract;
        entry.state.created = stamp;
        entry.state.latest = stamp;
        entry.state.attachment = id.mode == AYNI_CALL_MODE_MANAGED ? CallAttachment::background
                                                                   : CallAttachment::foreground;
        if (contract.subscription) {
            entry.state.subscription = SubscriptionFact{};
            entry.state.subscription->generation =
                id.operation_id; // Unique collection and per-generation operation identity.
        }
        entry.domain_size = domain_body.size;
        entry.input_size = input_body.size;
        entry.result_size = 0;
        entry.result_kind = CallResultKind::text;
        copy_bodies(at, domain_body, input_body, {});
        auto *proof = bytes(at) + limits_.domain_bytes + limits_.input_bytes + limits_.result_bytes;
        if (contract.business_contract.size != 0U) {
            std::memcpy(proof, contract.business_contract.data, contract.business_contract.size);
        }
        entry.contract.business_contract = {proof, contract.business_contract.size};
        ++size_;
        index = at;
        return AYNI_OK;
    }
    return AYNI_INTERNAL;
}

void CallTable::attach_execution(uint32_t index, const ExecutionFact &fact) {
    slots_[index].entry->state.execution = fact;
}
void CallTable::set_runtime_retention(const void *context,
                                      bool (*retains)(const void *, uint64_t)) {
    retention_context_ = context;
    runtime_retains_ = retains;
}
ayni_status_t CallTable::confirm_execution(uint32_t index, ayni_execution_phase_t phase,
                                           ayni_control_stop_cause_t cause,
                                           ayni_execution_target_t target, ActionStamp stamp) {
    auto &entry = *slots_[index].entry;
    if (!entry.state.execution || execution_stopped(*entry.state.execution) ||
        !advances(entry, stamp) ||
        (phase != AYNI_EXECUTION_STOPPED && phase != AYNI_EXECUTION_NOT_STARTED) ||
        (phase == AYNI_EXECUTION_STOPPED
             ? cause == AYNI_CONTROL_STOP_NONE || cause > AYNI_CONTROL_STOP_CLOCK
             : cause != AYNI_CONTROL_STOP_NONE || target != AYNI_TARGET_UNVERIFIED) ||
        target > AYNI_TARGET_UNSATISFIED) {
        return AYNI_INVALID_STATE;
    }
    auto &execution = *entry.state.execution;
    execution.phase = phase;
    execution.stop_cause = cause;
    execution.target = target;
    entry.state.latest = stamp;
    return AYNI_OK;
}

ayni_status_t CallTable::prepare(uint32_t index, ActionStamp stamp) {
    auto &entry = *slots_[index].entry;
    if (!pending(entry) || !advances(entry, stamp) || entry.action) {
        return AYNI_INVALID_STATE;
    }
    const auto &id = entry.identity;
    const auto status = ActionRecord::prepare(
        {entry.state.subscription ? entry.state.subscription->generation : id.operation_id,
         id.call_id, id.binding_id, id.binding_version},
        stamp, entry.action);
    if (status == AYNI_OK) {
        entry.state.latest = stamp;
    }
    return status;
}

ayni_status_t CallTable::begin_dispatch(uint32_t index, ayni_attempt_ref_v1 attempt,
                                        ActionStamp stamp, uint64_t durable_seq) {
    auto &entry = *slots_[index].entry;
    if (!pending(entry) || !advances(entry, stamp) || !entry.action ||
        entry.state.attempt.request_id != 0U || attempt.request_id == 0U ||
        attempt.boot_epoch != stamp.boot_epoch) {
        return AYNI_INVALID_STATE;
    }
    const auto status = entry.action->begin_dispatch(stamp, durable_seq);
    if (status == AYNI_OK) {
        entry.state.attempt = attempt;
        entry.state.latest = stamp;
        if (entry.state.execution) {
            entry.state.execution->phase = AYNI_EXECUTION_DISPATCHING;
        }
    }
    return status;
}

bool CallTable::claim_dispatch(uint32_t index, uint64_t boot_epoch, uint64_t durable_seq) {
    auto &entry = *slots_[index].entry;
    return pending(entry) && entry.action && entry.state.cancel_cause == CallCancelCause::none &&
           entry.action->claim_dispatch(boot_epoch, durable_seq);
}

void CallTable::set_terminal(Entry &entry, const CallResult &result, ActionStamp stamp) {
    entry.state.latest = stamp;
    entry.state.outcome = result.outcome;
    entry.state.status = result.status;
    entry.state.reason = result.reason;
    entry.state.model_usage = result.model_usage;
    entry.result_size = result.text.size;
    entry.result_kind = result.kind;
    if (entry.state.subscription) {
        auto &subscription = *entry.state.subscription;
        entry.state.attachment = CallAttachment::background;
        if (subscription.control_status != AYNI_OK) {
            subscription.phase = SubscriptionPhase::failed;
        } else if (subscription.phase == SubscriptionPhase::expired ||
                   result.status == AYNI_EXPIRED) {
            subscription.phase = SubscriptionPhase::expired;
            subscription.desired = SubscriptionDesired::closed;
        } else if (result.outcome == AYNI_CALL_CANCELLED) {
            subscription.phase = subscription.phase == SubscriptionPhase::expired
                                     ? SubscriptionPhase::expired
                                     : SubscriptionPhase::stopped;
            if (entry.state.cancel_cause == CallCancelCause::none) {
                subscription.desired = SubscriptionDesired::closed;
            }
        } else if (result.outcome == AYNI_CALL_COMPLETED) {
            subscription.phase = SubscriptionPhase::stopped;
            subscription.desired = SubscriptionDesired::closed;
        } else {
            subscription.phase = SubscriptionPhase::failed;
            subscription.desired = SubscriptionDesired::closed;
        }
    }
}

ayni_status_t CallTable::cancel_before_dispatch(uint32_t index, ActionStamp stamp) {
    return end_before_dispatch(
        index, {AYNI_CALL_CANCELLED, AYNI_OK, AYNI_REASON_NONE, ActionEffect::none, {}}, stamp);
}

ayni_status_t CallTable::skip_before_dispatch(uint32_t index, ActionStamp stamp) {
    const auto &state = slots_[index].entry->state;
    if (state.attempt.request_id != 0U || state.cancel_cause != CallCancelCause::none) {
        return AYNI_INVALID_STATE; // SKIPPED is a queue outcome, never a possible dispatch result.
    }
    return end_before_dispatch(
        index, {AYNI_CALL_SKIPPED, AYNI_EXPIRED, AYNI_REASON_RESOURCE_WAIT, ActionEffect::none, {}},
        stamp);
}

ayni_status_t CallTable::end_before_dispatch(uint32_t index, const CallResult &result,
                                             ActionStamp stamp) {
    auto &entry = *slots_[index].entry;
    if (!pending(entry) || !advances(entry, stamp)) {
        return AYNI_INVALID_STATE;
    }
    if (entry.action) {
        const auto status = entry.action->cancel_before_delivery(stamp);
        if (status != AYNI_OK) {
            return status;
        }
    }
    set_terminal(entry, result, stamp);
    if (entry.state.execution) {
        entry.state.execution->phase = AYNI_EXECUTION_NOT_STARTED;
    }
    return AYNI_OK;
}

ayni_status_t CallTable::rejected_not_retained(uint32_t index, ActionStamp stamp) {
    auto &entry = *slots_[index].entry;
    if (!pending(entry) || !advances(entry, stamp) || !entry.action) {
        return AYNI_INVALID_STATE;
    }
    const auto status = entry.action->rejected_not_retained(stamp);
    if (status == AYNI_OK) {
        set_terminal(entry,
                     {AYNI_CALL_FAILED, AYNI_NOT_READY, AYNI_REASON_NONE, ActionEffect::none, {}},
                     stamp);
    }
    return status;
}

ayni_status_t CallTable::request_cancel(uint32_t index, CallCancelCause cause, ActionStamp stamp) {
    auto &entry = *slots_[index].entry;
    if (cause != CallCancelCause::user && cause != CallCancelCause::authority &&
        cause != CallCancelCause::deadline && cause != CallCancelCause::shutdown) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (!pending(entry) || !advances(entry, stamp) ||
        entry.state.cancel_cause != CallCancelCause::none) {
        return AYNI_INVALID_STATE;
    }
    entry.state.cancel_cause = cause;
    entry.state.latest = stamp;
    return AYNI_OK;
}

ayni_status_t CallTable::cancel_response(uint32_t index, ayni_call_outcome_t outcome,
                                         ActionStamp stamp) {
    auto &entry = *slots_[index].entry;
    if (outcome != AYNI_CALL_CANNOT_CANCEL && outcome != AYNI_CALL_CANCEL_UNKNOWN) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (!pending(entry) || !advances(entry, stamp) ||
        entry.state.cancel_cause == CallCancelCause::none || entry.state.cancel_result != 0U ||
        entry.state.attempt.request_id == 0U) {
        return AYNI_INVALID_STATE;
    }
    entry.state.cancel_result = outcome;
    entry.state.latest = stamp;
    return AYNI_OK;
}

ayni_status_t CallTable::progress(uint32_t index, uint64_t sequence, ActionStamp stamp) {
    auto &entry = *slots_[index].entry;
    if (!pending(entry) || !advances(entry, stamp) || !entry.action ||
        sequence <= entry.state.progress_sequence) {
        return AYNI_INVALID_STATE;
    }
    const auto status = entry.action->progress(stamp);
    if (status == AYNI_OK) {
        entry.state.progress_sequence = sequence;
        entry.state.latest = stamp;
    }
    return status;
}

// Text/shape validation precedes the ledger's only transition check. Once
// that succeeds, copying the result and publishing call state cannot fail.
ayni_status_t CallTable::finish(ayni::foundation::Workspace &workspace, uint32_t index,
                                const CallResult &input, ActionStamp stamp,
                                ResultDependencies dependencies) {
    auto &entry = *slots_[index].entry;
    const auto checked = check_result(workspace, index, input);
    if (checked != AYNI_OK) {
        return checked;
    }
    auto result = usable_result(input, dependencies);
    if (entry.state.content_erased) {
        result.text = {};
        result.kind = CallResultKind::text;
    }
    if (!pending(entry) || !advances(entry, stamp) || !entry.action) {
        return AYNI_INVALID_STATE;
    }
    const auto status = finish_action(*entry.action, result, stamp);
    if (status != AYNI_OK) {
        return status;
    }
    if (result.text.size != 0U) {
        std::memcpy(bytes(index) + limits_.domain_bytes + limits_.input_bytes, result.text.data,
                    result.text.size);
    }
    std::memset(bytes(index) + limits_.domain_bytes + limits_.input_bytes + result.text.size, 0,
                limits_.result_bytes - result.text.size);
    set_terminal(entry, result, stamp);
    return AYNI_OK;
}

// The original input ceases to exist as a body, not as a fictional successful/failed action.
void CallTable::discard_result(uint32_t index) {
    auto &entry = *slots_[index].entry;
    std::memset(bytes(index) + limits_.domain_bytes + limits_.input_bytes, 0, limits_.result_bytes);
    entry.result_size = 0;
    entry.result_kind = CallResultKind::text;
}

void CallTable::discard_unusable_result(uint32_t index) {
    discard_result(index);
    slots_[index].entry->result_kind = CallResultKind::unavailable;
}

ayni_status_t CallTable::erase_content(uint32_t index, ActionStamp stamp) {
    auto &entry = *slots_[index].entry;
    if (entry.state.content_erased || !advances(entry, stamp)) {
        return AYNI_INVALID_STATE;
    }
    std::memset(bytes(index) + limits_.domain_bytes, 0, limits_.input_bytes + limits_.result_bytes);
    entry.input_size = 0;
    entry.result_size = 0;
    entry.result_kind = CallResultKind::text;
    entry.state.content_erased = stamp;
    return AYNI_OK;
}

bool CallTable::can_retire_task(uint64_t task_id, uint64_t durable_seq) const {
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        const auto &entry = slots_[at].entry;
        if (entry && entry->identity.task_id == task_id &&
            (pending(*entry) || unresolved(at) ||
             (entry->state.execution && !execution_stopped(*entry->state.execution)) ||
             (runtime_retains_ != nullptr &&
              runtime_retains_(retention_context_, entry->identity.call_id)) ||
             entry->state.latest.transition.commit_seq > durable_seq)) {
            return false;
        }
    }
    return true;
}

void CallTable::retire_task(uint64_t task_id) {
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        auto &entry = slots_[at].entry;
        if (entry && entry->identity.task_id == task_id) {
            std::memset(bytes(at), 0,
                        static_cast<std::size_t>(limits_.domain_bytes) + limits_.input_bytes +
                            limits_.result_bytes + limits_.business_bytes);
            entry.reset();
            --size_;
        }
    }
}

bool CallTable::find(uint64_t call_id, uint32_t &index) const {
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        if (occupied(at) && slots_[at].entry->identity.call_id == call_id) {
            index = at;
            return true;
        }
    }
    return false;
}

bool CallTable::find_step(uint64_t task_id, ayni_string_v1 key, uint32_t &index) const {
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        if (occupied(at) && slots_[at].entry->identity.task_id == task_id &&
            slots_[at].entry->identity.step_key.equals(key)) {
            index = at;
            return true;
        }
    }
    return false;
}

bool CallTable::occupied(uint32_t index) const {
    return index < limits_.capacity && slots_[index].entry.has_value();
}
const CallIdentity &CallTable::identity(uint32_t index) const {
    return slots_[index].entry->identity;
}
const catalog::CallContract &CallTable::contract(uint32_t index) const {
    return slots_[index].entry->contract;
}
const CallState &CallTable::state(uint32_t index) const {
    return slots_[index].entry->state;
}
const ActionFact *CallTable::action(uint32_t index) const {
    const auto &action = slots_[index].entry->action;
    return action ? &action->fact() : nullptr;
}
ayni_bytes_v1 CallTable::domain(uint32_t index) const {
    return {bytes(index), slots_[index].entry->domain_size};
}
ayni_bytes_v1 CallTable::input(uint32_t index) const {
    return {bytes(index) + limits_.domain_bytes, slots_[index].entry->input_size};
}
ayni_string_v1 CallTable::body(uint32_t index) const {
    return {
        reinterpret_cast<const char *>(bytes(index) + limits_.domain_bytes + limits_.input_bytes),
        slots_[index].entry->result_size};
}
ayni_string_v1 CallTable::result(uint32_t index) const {
    return slots_[index].entry->result_kind == CallResultKind::text ? body(index)
                                                                    : ayni_string_v1{};
}
ayni_bytes_v1 CallTable::tool_calls(uint32_t index) const {
    if (slots_[index].entry->result_kind != CallResultKind::tool_calls) {
        return {};
    }
    const auto list = body(index);
    return {reinterpret_cast<const uint8_t *>(list.data), list.size};
}
ayni_bytes_v1 CallTable::value_result(uint32_t index) const {
    if (slots_[index].entry->result_kind != CallResultKind::value) {
        return {};
    }
    const auto value = body(index);
    return {reinterpret_cast<const uint8_t *>(value.data), value.size};
}
} // namespace ayni::work
