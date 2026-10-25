#include "work/call/call_table.h"

#include "foundation/kernel_id.h"

#include <cstring>

namespace ayni::work {
namespace {
bool same_stamp(ActionStamp left, ActionStamp right) {
    return left.boot_epoch == right.boot_epoch &&
           left.transition.transition_id == right.transition.transition_id &&
           left.transition.commit_seq == right.transition.commit_seq;
}

bool at_or_before(ActionStamp before, ActionStamp after) {
    return same_stamp(before, after) ||
           (before.boot_epoch <= after.boot_epoch &&
            before.transition.transition_id < after.transition.transition_id &&
            before.transition.commit_seq < after.transition.commit_seq);
}

bool same_id(const catalog::CatalogId &left, const catalog::CatalogId &right) {
    return left.equals({right.data(), right.size()});
}

bool same_identity(const CallIdentity &left, const CallIdentity &right) {
    return left.call_id == right.call_id && left.operation_id == right.operation_id &&
           left.task_id == right.task_id && left.owner_instance_id == right.owner_instance_id &&
           same_id(left.step_key, right.step_key) &&
           catalog::BindingRegistry::same_selection(left.binding(), right.binding());
}

bool same_bytes(ayni_bytes_v1 left, ayni_bytes_v1 right) {
    return left.size == right.size &&
           (left.size == 0U || std::memcmp(left.data, right.data, left.size) == 0);
}

// Delivery outcomes may conclude local tracking without resolving remote effects.
// A queue cancellation has no action fact; NOT_RETAINED has local non-delivery evidence.
bool result_matches(const CallRecord &record) {
    const auto &state = record.state;
    const auto *action = record.action ? &*record.action : nullptr;
    switch (state.outcome) {
    case 0:
        return record.result.size == 0U && state.status == AYNI_OK &&
               state.reason == AYNI_REASON_NONE &&
               (action == nullptr || action->phase == ActionPhase::prepared ||
                action->phase == ActionPhase::dispatching ||
                action->phase == ActionPhase::in_progress);
    case AYNI_CALL_COMPLETED:
        return state.status == AYNI_OK && state.reason == AYNI_REASON_NONE && action != nullptr &&
               action->phase == ActionPhase::completed;
    case AYNI_CALL_FAILED:
        return state.status < AYNI_OK && state.status != AYNI_RESULT_UNKNOWN && action != nullptr &&
               (action->phase == ActionPhase::failed ||
                (action->phase == ActionPhase::cancelled &&
                 action->evidence == ActionEvidence::local_non_delivery &&
                 state.status == AYNI_NOT_READY));
    case AYNI_CALL_CANCELLED:
        return state.status == AYNI_OK && state.reason == AYNI_REASON_NONE &&
               (action == nullptr || action->phase == ActionPhase::cancelled);
    case AYNI_CALL_ABANDONED:
        return state.status == AYNI_RESULT_UNKNOWN && record.result.size == 0U &&
               action != nullptr && action->phase == ActionPhase::result_unknown;
    default:
        return false;
    }
}

bool action_matches(const CallRecord &record) {
    if (!record.action) {
        return record.state.attempt.request_id == 0U && record.state.progress_sequence == 0U;
    }
    const auto &action = *record.action;
    const auto &id = record.identity;
    const auto &state = record.state;
    if (!ActionRecord::valid(action) || action.identity.operation_id != id.operation_id ||
        action.identity.logical_call_id != id.call_id ||
        action.identity.binding_version != id.binding_version ||
        !same_id(action.identity.binding_id, id.binding_id) ||
        same_stamp(state.created, action.prepared) ||
        !at_or_before(state.created, action.prepared) ||
        !at_or_before(action.latest, state.latest)) {
        return false;
    }
    const bool has_attempt = state.attempt.request_id != 0U;
    if (action.phase == ActionPhase::prepared) {
        return !has_attempt && state.progress_sequence == 0U;
    }
    // Final local rejection can precede or follow the durable dispatch barrier.
    return has_attempt || (action.phase == ActionPhase::cancelled &&
                           action.evidence == ActionEvidence::local_non_delivery);
}
} // namespace

// Decode and live admission share the canonical input validator and contract
// normalizer. Original schemas are never substituted by this boot's registration.
bool CallTable::valid_record(const CallRecord &record) {
    const auto &state = record.state;
    catalog::CallContract normalized{};
    CallInputView input{};
    if (!valid_identity(record.identity, state.created) ||
        !at_or_before(state.created, state.latest) ||
        state.latest.boot_epoch > foundation::max_boot_epoch ||
        ((record.contract.effects & catalog::call_effect_egress) != 0U) !=
            (record.identity.destination.size() != 0U) ||
        catalog::normalize_call_contract(record.contract, normalized) != AYNI_OK ||
        !catalog::same_call_contract(record.contract, normalized) ||
        read_call_input(record.domain, record.input, record.contract.input_text_limit, input) !=
            AYNI_OK ||
        check_call_text(record.result, record.contract.result_text_limit) != AYNI_OK ||
        (state.attempt.boot_epoch == 0U) != (state.attempt.request_id == 0U) ||
        (state.attempt.request_id != 0U && (state.attempt.boot_epoch < state.created.boot_epoch ||
                                            state.attempt.boot_epoch > state.latest.boot_epoch)) ||
        state.cancel_cause > CallCancelCause::shutdown ||
        (state.cancel_result != 0U &&
         (state.cancel_cause == CallCancelCause::none || state.attempt.request_id == 0U ||
          (state.cancel_result != AYNI_CALL_CANNOT_CANCEL &&
           state.cancel_result != AYNI_CALL_CANCEL_UNKNOWN)))) {
        return false;
    }
    return action_matches(record) && result_matches(record) && valid_query(record);
}

CallRecord CallTable::record(uint32_t index) const {
    const auto &entry = *slots_[index].entry;
    return {entry.identity,
            entry.contract,
            entry.state,
            entry.action ? std::optional<ActionFact>(entry.action->fact()) : std::nullopt,
            domain(index),
            input(index),
            result(index)};
}

void CallTable::copy_bodies(uint32_t index, ayni_bytes_v1 domain_body, ayni_bytes_v1 input_body,
                            ayni_string_v1 result_body) {
    std::memcpy(bytes(index), domain_body.data, domain_body.size);
    std::memcpy(bytes(index) + limits_.domain_bytes, input_body.data, input_body.size);
    if (result_body.size != 0U) {
        std::memcpy(bytes(index) + limits_.domain_bytes + limits_.input_bytes, result_body.data,
                    result_body.size);
    }
}

// Replay can replace state only for the same immutable operation and request.
// A new row may be a retained snapshot terminal; a journal consumer separately
// requires a creation row before subsequent updates and checks batch pairing.
ayni_status_t CallTable::restore_validated(const CallRecord &record) {
    if (record.domain.size > limits_.domain_bytes || record.input.size > limits_.input_bytes ||
        record.contract.result_text_limit > limits_.result_bytes) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    uint32_t index = 0;
    if (!find(record.identity.call_id, index)) {
        const auto status = insert(record.identity, record.contract, record.domain, record.input,
                                   record.state.created, index);
        if (status != AYNI_OK) {
            return status;
        }
    } else {
        const auto &entry = *slots_[index].entry;
        const auto &before = entry.state;
        const auto &after = record.state;
        if (!same_identity(entry.identity, record.identity) ||
            !catalog::same_call_contract(entry.contract, record.contract) ||
            !same_bytes(domain(index), record.domain) || !same_bytes(input(index), record.input) ||
            !same_stamp(before.created, after.created) || !advances(entry, after.latest) ||
            (before.outcome != 0U && !unresolved(index)) || !query_advances(before, after) ||
            after.progress_sequence < before.progress_sequence ||
            (before.attempt.request_id != 0U &&
             (before.attempt.request_id != after.attempt.request_id ||
              before.attempt.boot_epoch != after.attempt.boot_epoch)) ||
            (before.cancel_cause != CallCancelCause::none &&
             before.cancel_cause != after.cancel_cause) ||
            (before.cancel_result != 0U && before.cancel_result != after.cancel_result) ||
            (entry.action && (!record.action || !same_stamp(entry.action->fact().prepared,
                                                            record.action->prepared)))) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    auto &entry = *slots_[index].entry;
    entry.state = record.state;
    entry.result_size = record.result.size;
    if (record.action) {
        entry.action.emplace();
        entry.action->restore_validated(*record.action);
    }
    // Immutable bodies already reside in this slot; only the terminal body changes.
    if (record.result.size != 0U) {
        std::memcpy(bytes(index) + limits_.domain_bytes + limits_.input_bytes, record.result.data,
                    record.result.size);
    }
    return AYNI_OK;
}

void CallTable::clear() {
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        if (occupied(at)) {
            retire_task(identity(at).task_id);
        }
    }
}
} // namespace ayni::work
