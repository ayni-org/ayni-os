#include "foundation/workspace.h"
#include "work/call/call_table.h"

#include "catalog/capability/business_encoding.h"
#include "catalog/capability/value_validation.h"
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
    return left.mode == right.mode && left.model_context == right.model_context &&
           left.authorization.grant_id == right.authorization.grant_id &&
           left.authorization.revision == right.authorization.revision &&
           left.call_id == right.call_id && left.operation_id == right.operation_id &&
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
    case AYNI_CALL_SKIPPED:
        return state.status == AYNI_EXPIRED && state.reason == AYNI_REASON_RESOURCE_WAIT &&
               !state.query && state.cancel_cause == CallCancelCause::none &&
               state.attempt.request_id == 0U && record.result.size == 0U &&
               (action == nullptr || (action->phase == ActionPhase::cancelled &&
                                      action->evidence == ActionEvidence::local_non_delivery));
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
    if (!ActionRecord::valid(action) ||
        action.identity.operation_id !=
            (state.subscription ? state.subscription->generation : id.operation_id) ||
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
// Saved business interpretation includes original semantics and a unit-preserving resource
// substitution (ADR-0087), normalized defaults
// and a present typed result unless its body was explicitly made unavailable.
bool valid_business_content(ayni::foundation::Workspace &workspace, const CallRecord &record) {
    const auto &state = record.state;
    if ((saved_value_input(record.contract) ||
         record.contract.result_format == catalog::call_value_result_format) &&
        record.contract.business_contract.size == 0U) {
        return false;
    }
    if (record.contract.business_contract.size != 0U) {
        auto proof_lease = workspace.make<catalog::DecodedBusiness>();
        if (!proof_lease) {
            return false;
        }
        auto &proof = *proof_lease;
        if (catalog::decode_business(workspace, record.domain, record.contract.business_contract,
                                     proof) != AYNI_OK ||
            !catalog::bound_call_contract(proof.execution, record.contract)) {
            return false;
        }
        if (!state.content_erased && record.result_kind == CallResultKind::text &&
            state.result_body.id == 0U &&
            (state.outcome == AYNI_CALL_COMPLETED || record.result.size != 0U)) {
            ayni_value_node_v1 text{};
            text.kind = AYNI_VALUE_TEXT;
            text.subtree_end = 1;
            text.value.text = record.result;
            if (!catalog::scalar_matches(proof.result.nodes[0], text)) {
                return false;
            }
        }
        if (record.result_kind == CallResultKind::value && record.state.result_body.id == 0U) {
            auto result_lease = workspace.make<catalog::DecodedValue>();
            if (!result_lease) {
                return false;
            }
            auto &result = *result_lease;
            if (catalog::read_normalized_value(
                    workspace, proof.result.view(),
                    {reinterpret_cast<const uint8_t *>(record.result.data), record.result.size},
                    result) != AYNI_OK) {
                return false;
            }
        }
        if (!state.content_erased && state.outcome == AYNI_CALL_COMPLETED &&
            record.contract.result_format == catalog::call_value_result_format &&
            record.result_kind != CallResultKind::value &&
            record.result_kind != CallResultKind::unavailable) {
            return false;
        }
    }
    return true;
}

// An explicit privacy record removes input validation material; it must contain no body and
// retains only independently valid schema/execution facts. Old readers refuse this record version.
bool valid_content(ayni::foundation::Workspace &workspace, const CallRecord &record,
                   CallInputView &input) {
    if (!record.state.content_erased) {
        return read_call_input(workspace, record.domain, record.input, record.contract, input) ==
               AYNI_OK;
    }
    const auto &erased = *record.state.content_erased;
    if (record.input.size != 0 || record.result.size != 0 ||
        record.result_kind != CallResultKind::text || !at_or_before(record.state.created, erased) ||
        same_stamp(record.state.created, erased) ||
        erased.boot_epoch > foundation::max_boot_epoch) {
        return false;
    }
    catalog::DomainReader reader(record.domain);
    catalog::DomainField field{};
    while (reader.next(field)) {
    }
    return reader.ok();
}
// Only this transition permits replacing the prior generation's sealed action/attempt.
// A background known close is not an unresolved unary/action retry.
bool reopens_subscription(const CallState &before, const CallState &after) {
    return before.subscription && after.subscription && before.outcome == AYNI_CALL_CANCELLED &&
           after.outcome == 0 && after.attempt.request_id == 0 &&
           after.cancel_cause == CallCancelCause::none && after.cancel_result == 0 &&
           after.progress_sequence == 0 &&
           after.subscription->generation > before.subscription->generation &&
           subscription_advances(*before.subscription, *after.subscription);
}
bool subscriptions_advance(const CallState &before, const CallState &after) {
    return before.subscription.has_value() == after.subscription.has_value() &&
           (!before.subscription ||
            subscription_advances(*before.subscription, *after.subscription));
}
// After a known terminal, only desired intent may close. The result/action proof
// is immutable until a separately validated new generation is established.
bool known_subscription_unchanged(const CallRecord &before, const CallRecord &after) {
    const auto &a = before.state;
    const auto &b = after.state;
    if (a.outcome != b.outcome || a.status != b.status || a.reason != b.reason ||
        a.progress_sequence != b.progress_sequence ||
        !same_bytes({reinterpret_cast<const uint8_t *>(before.result.data), before.result.size},
                    {reinterpret_cast<const uint8_t *>(after.result.data), after.result.size}) ||
        before.result_kind != after.result_kind ||
        before.action.has_value() != after.action.has_value() ||
        a.subscription->phase != b.subscription->phase ||
        a.subscription->lease_sequence != b.subscription->lease_sequence ||
        a.subscription->control_status != b.subscription->control_status ||
        a.subscription->control_reason != b.subscription->control_reason) {
        return false;
    }
    if (!before.action) {
        return true;
    }
    return before.action->phase == after.action->phase &&
           before.action->effect == after.action->effect &&
           before.action->evidence == after.action->evidence &&
           same_stamp(before.action->latest, after.action->latest);
}
bool same_erasure(const CallState &before, const CallState &after) {
    return before.content_erased.has_value() == after.content_erased.has_value() &&
           (!before.content_erased || same_stamp(*before.content_erased, *after.content_erased));
}
} // namespace

// A lease confirmation cannot exist without the generation's own dispatch proof.
bool valid_subscription_record(const CallRecord &record) {
    const auto &state = record.state;
    if (record.contract.subscription.has_value() != state.subscription.has_value()) {
        return false;
    }
    if (!state.subscription) {
        return true;
    }
    const auto &fact = *state.subscription;
    if (!valid_subscription(fact) ||
        (fact.phase == SubscriptionPhase::active &&
         state.attachment != CallAttachment::background) ||
        foundation::id_boot_epoch(fact.generation) > state.latest.boot_epoch ||
        fact.generation < record.identity.operation_id) {
        return false;
    }
    if (fact.lease_sequence != 0 && (!record.action || state.attempt.request_id == 0 ||
                                     record.action->phase == ActionPhase::prepared ||
                                     record.action->phase == ActionPhase::dispatching)) {
        return false;
    }
    if (fact.phase == SubscriptionPhase::active &&
        (state.outcome != 0 || state.cancel_cause != CallCancelCause::none)) {
        return false;
    }
    if (fact.phase == SubscriptionPhase::starting && state.cancel_cause != CallCancelCause::none) {
        return false;
    }
    if ((fact.phase == SubscriptionPhase::stopping || fact.phase == SubscriptionPhase::expired) &&
        state.cancel_cause == CallCancelCause::none && state.outcome == 0) {
        return false;
    }
    return fact.phase != SubscriptionPhase::stopped ||
           (state.outcome != 0 && state.outcome != AYNI_CALL_ABANDONED);
}

// Decode and live admission share the canonical input validator and contract
// normalizer. Original schemas are never substituted by this boot's registration.
// Saved external results retain the original normalized kind and proof. The consumer verifies
// the immutable body bytes; replay validates reference shape without platform I/O (ADR-0104).
bool CallTable::valid_result_body(foundation::Workspace &workspace, const CallRecord &record) {
    const auto &state = record.state;
    return !((
        state.result_body.id == 0U
            ? state.result_body.digest != 0U || state.result_body.size != 0U ||
                  state.result_body_calls != 0U ||
                  check_body(workspace, record.contract, record.result, record.result_kind) !=
                      AYNI_OK
            : state.outcome != AYNI_CALL_COMPLETED || record.result.size != 0U ||
                  record.state.content_erased ||
                  (record.contract.category == catalog::CallClass::model &&
                   record.contract.result_format == catalog::call_value_result_format) ||
                  state.result_body_calls > AYNI_CALL_TOOL_COUNT_MAX ||
                  (record.contract.category == catalog::CallClass::model
                       ? record.result_kind != (state.result_body_calls ? CallResultKind::tool_calls
                                                                        : CallResultKind::text)
                       : state.result_body_calls != 0U ||
                             (record.result_kind != CallResultKind::text &&
                              record.result_kind != CallResultKind::value) ||
                             (record.result_kind == CallResultKind::value &&
                              record.contract.result_format != catalog::call_value_result_format) ||
                             state.result_body.size > record.contract.result_text_limit) ||
                  (state.result_body_calls != 0U &&
                   record.contract.result_format != catalog::call_tool_calls_result_format)));
}

bool CallTable::valid_record(ayni::foundation::Workspace &workspace, const CallRecord &record) {
    const auto &state = record.state;
    catalog::CallContract normalized{};
    auto input_lease = workspace.make<CallInputView>();
    if (!input_lease) {
        return false;
    }
    auto &input = *input_lease;
    if (!valid_identity(record.identity, state.created) ||
        !valid_mode(record.identity, record.contract) ||
        (record.identity.mode == AYNI_CALL_MODE_MANAGED &&
         state.attachment != CallAttachment::background) ||
        (state.attachment != CallAttachment::foreground &&
         state.attachment != CallAttachment::background) ||
        (record.identity.mode == AYNI_CALL_MODE_FOREGROUND && !state.subscription &&
         state.attachment != CallAttachment::foreground) ||
        !at_or_before(state.created, state.latest) ||
        state.latest.boot_epoch > foundation::max_boot_epoch ||
        !valid_model_usage(state.model_usage) ||
        (state.model_usage.kind != AYNI_MODEL_USAGE_NONE &&
         (record.contract.category != catalog::CallClass::model || state.outcome == 0)) ||
        ((record.contract.effects & catalog::call_effect_egress) != 0U) !=
            (record.identity.destination.size() != 0U) ||
        catalog::normalize_call_contract(record.contract, normalized) != AYNI_OK ||
        !catalog::same_call_contract(record.contract, normalized) ||
        !valid_business_content(workspace, record) || !valid_content(workspace, record, input) ||
        !valid_result_body(workspace, record) ||
        (record.result_kind == CallResultKind::tool_calls &&
         state.outcome != AYNI_CALL_COMPLETED) ||
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
    if (record.result_kind == CallResultKind::unavailable && state.outcome == 0U) {
        return false;
    }
    if (!valid_subscription_record(record)) {
        return false;
    }
    if (((record.contract.effects & catalog::call_effect_physical) != 0) !=
        state.execution.has_value()) {
        return false;
    }
    // Physical rows retain their original permit constraints; non-physical rows have no execution
    // fact. Neither replay nor a new catalog may infer a fresh execution generation.
    if (state.execution) {
        const auto &execution = *state.execution;
        if (!(state.content_erased ? valid_execution_metadata(execution)
                                   : valid_execution(execution, input.arguments)) ||
            execution.epochs.kernel_boot_epoch != state.created.boot_epoch ||
            execution.scene_seq >= state.created.transition.commit_seq ||
            !execution.binding.binding.equals(
                {record.identity.binding_id.data(), record.identity.binding_id.size()}) ||
            execution.binding.binding_version != record.identity.binding_version ||
            (execution.phase == AYNI_EXECUTION_PREPARED && state.attempt.request_id != 0) ||
            (execution.phase == AYNI_EXECUTION_DISPATCHING && state.attempt.request_id == 0)) {
            return false;
        }
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
            body(index),
            entry.result_kind};
}

void CallTable::copy_bodies(uint32_t index, ayni_bytes_v1 domain_body, ayni_bytes_v1 input_body,
                            ayni_string_v1 result_body) {
    std::memcpy(bytes(index), domain_body.data, domain_body.size);
    std::memset(bytes(index) + domain_body.size, 0, limits_.domain_bytes - domain_body.size);
    if (input_body.size != 0) {
        std::memcpy(bytes(index) + limits_.domain_bytes, input_body.data, input_body.size);
    }
    std::memset(bytes(index) + limits_.domain_bytes + input_body.size, 0,
                limits_.input_bytes - input_body.size);
    if (result_body.size != 0U) {
        std::memcpy(bytes(index) + limits_.domain_bytes + limits_.input_bytes, result_body.data,
                    result_body.size);
    }
    std::memset(bytes(index) + limits_.domain_bytes + limits_.input_bytes + result_body.size, 0,
                limits_.result_bytes - result_body.size);
}

// Replay can replace state only for the same immutable operation and request.
// A new row may be a retained snapshot terminal; a journal consumer separately
// requires a creation row before subsequent updates and checks batch pairing.
ayni_status_t CallTable::restore_validated(const CallRecord &record) {
    if (record.domain.size > limits_.domain_bytes || record.input.size > limits_.input_bytes ||
        record.contract.result_text_limit > limits_.result_bytes ||
        record.contract.business_contract.size > limits_.business_bytes) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    uint32_t index = 0;
    if (!find(record.identity.call_id, index)) {
        const auto status = place_new(record.identity, record.contract, record.domain, record.input,
                                      record.state.created, index);
        if (status != AYNI_OK) {
            return status;
        }
    } else {
        const auto &entry = *slots_[index].entry;
        const auto &before = entry.state;
        const auto &after = record.state;
        const bool reopening = reopens_subscription(before, after);
        if (!same_identity(entry.identity, record.identity) ||
            !catalog::same_call_contract(entry.contract, record.contract) ||
            !same_bytes(domain(index), record.domain) || !same_bytes(input(index), record.input) ||
            !same_stamp(before.created, after.created) || !same_erasure(before, after) ||
            !advances(entry, after.latest) ||
            (before.outcome != 0U && !unresolved(index) && !reopening &&
             (!before.subscription ||
              !known_subscription_unchanged(this->record(index), record))) ||
            // ADR-0063: confirmed subscription closure permits a new generation,
            // not a query of the old operation. Its fresh state has no query.
            (reopening ? after.query.has_value() : !query_advances(before, after)) ||
            !subscriptions_advance(before, after) ||
            (before.attachment == CallAttachment::background &&
             after.attachment != before.attachment) ||
            !execution_advances(before.execution, after.execution) ||
            (!reopening && after.progress_sequence < before.progress_sequence) ||
            (!reopening && before.attempt.request_id != 0U &&
             (before.attempt.request_id != after.attempt.request_id ||
              before.attempt.boot_epoch != after.attempt.boot_epoch)) ||
            (!reopening && before.cancel_cause != CallCancelCause::none &&
             before.cancel_cause != after.cancel_cause) ||
            (!reopening && before.cancel_result != 0U &&
             before.cancel_result != after.cancel_result) ||
            (!reopening && entry.action &&
             (!record.action ||
              !same_stamp(entry.action->fact().prepared, record.action->prepared)))) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    auto &entry = *slots_[index].entry;
    entry.state = record.state;
    entry.result_size = record.result.size;
    entry.result_kind = record.result_kind;
    if (!record.action) {
        entry.action.reset();
    }
    if (record.action) {
        entry.action.emplace();
        entry.action->restore_validated(*record.action);
    }
    // Immutable bodies already reside in this slot; only the terminal body changes.
    if (record.result.size != 0U) {
        std::memcpy(bytes(index) + limits_.domain_bytes + limits_.input_bytes, record.result.data,
                    record.result.size);
    }
    std::memset(bytes(index) + limits_.domain_bytes + limits_.input_bytes + record.result.size, 0,
                limits_.result_bytes - record.result.size);
    return AYNI_OK;
}

// Pending releases may name rows a reload brings back, so they are forgotten.
void CallTable::clear() {
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        if (occupied(at)) {
            retire_task(identity(at).task_id, 0);
        }
        slots_[at].erased = {};
    }
    for (uint32_t at = 0; at < release_capacity(); ++at) {
        releases_[at] = {};
    }
    release_head_ = 0;
    release_count_ = 0;
}
} // namespace ayni::work
