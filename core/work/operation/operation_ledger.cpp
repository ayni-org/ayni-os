#include "work/operation/operation_ledger.h"
#include "foundation/attempt_identity.h"
#include "foundation/kernel_id.h"
#include <cstring>
#include <new>

namespace ayni::work {
namespace {
bool before(ActionStamp a, ActionStamp b) {
    return a.boot_epoch != 0 && a.boot_epoch <= b.boot_epoch &&
           a.transition.commit_seq < b.transition.commit_seq &&
           a.transition.transition_id < b.transition.transition_id;
}
bool same_stamp(ActionStamp a, ActionStamp b) {
    return a.boot_epoch == b.boot_epoch && a.transition.commit_seq == b.transition.commit_seq &&
           a.transition.transition_id == b.transition.transition_id;
}
OperationIdentity identity_from_call(const CallIdentity &id, const ActionFact &action) {
    return {id.call_id,     action.identity.operation_id, id.task_id,       id.owner_instance_id,
            id.binding_id,  id.binding_version,           id.capability_id, id.capability_version,
            id.provider_id, id.provider_version,          id.endpoint,      id.destination};
}
OperationRecord from_call(const CallIdentity &id, const catalog::CallContract &contract,
                          const CallState &state, const ActionFact &action, ActionStamp stamp) {
    OperationState saved{
        state.created, stamp,        stamp, state.attempt, state.outcome,
        state.status,  state.reason, {},    state.query,   query_revision(state.query)};
    if (state.execution) {
        const auto &stop = *state.execution;
        saved.execution_stop = ExecutionStopSummary{stop.phase, stop.stop_cause, stop.target};
    }
    return {identity_from_call(id, action), contract, saved, action};
}
bool valid_stop(const std::optional<ExecutionStopSummary> &stop) {
    return !stop ||
           ((stop->phase == AYNI_EXECUTION_STOPPED || stop->phase == AYNI_EXECUTION_NOT_STARTED) &&
            stop->stop_cause <= AYNI_CONTROL_STOP_CLOCK &&
            (stop->phase == AYNI_EXECUTION_STOPPED ? stop->stop_cause != AYNI_CONTROL_STOP_NONE
                                                   : stop->stop_cause == AYNI_CONTROL_STOP_NONE &&
                                                         stop->target == AYNI_TARGET_UNVERIFIED) &&
            stop->target <= AYNI_TARGET_UNSATISFIED);
}
} // namespace

std::size_t OperationLedger::storage_size(Limits limits) {
    return limits.capacity *
           (sizeof(Slot) + sizeof(PendingAudit) + static_cast<std::size_t>(limits.business_bytes));
}
OperationLedger::OperationLedger(void *storage, Limits limits)
    : slots_(static_cast<Slot *>(storage)),
      proofs_(limits.capacity == 0 ? nullptr
                                   : reinterpret_cast<uint8_t *>(slots_ + limits.capacity)),
      audits_(limits.capacity == 0 ? nullptr : reinterpret_cast<PendingAudit *>(proofs_)),
      limits_(limits) {
    // Audit metadata precedes byte strides, so an odd business_bytes never misaligns it.
    proofs_ =
        limits.capacity == 0 ? nullptr : reinterpret_cast<uint8_t *>(audits_ + limits.capacity);
    for (uint32_t at = 0; at < limits.capacity; ++at) {
        new (slots_ + at) Slot{};
        new (audits_ + at) PendingAudit{};
    }
}
uint8_t *OperationLedger::proof(uint32_t index) const {
    return proofs_ + static_cast<std::size_t>(index) * limits_.business_bytes;
}
bool OperationLedger::occupied(uint32_t index) const {
    return index < limits_.capacity && slots_[index].entry.has_value();
}
bool OperationLedger::find(uint64_t call_id, uint32_t &index) const {
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        if (occupied(at) && identity(at).call_id == call_id) {
            index = at;
            return true;
        }
    }
    return false;
}
bool OperationLedger::contains_identity(uint64_t id) const {
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        if (occupied(at) && (identity(at).call_id == id || identity(at).operation_id == id)) {
            return true;
        }
    }
    return false;
}
const OperationIdentity &OperationLedger::identity(uint32_t index) const {
    return slots_[index].entry->identity;
}
const catalog::CallContract &OperationLedger::contract(uint32_t index) const {
    return slots_[index].entry->contract;
}
const OperationState &OperationLedger::state(uint32_t index) const {
    return slots_[index].entry->state;
}
OperationRecord OperationLedger::record(uint32_t index) const {
    const auto &entry = *slots_[index].entry;
    return {entry.identity, entry.contract, entry.state, entry.action.fact()};
}
bool OperationLedger::query_pending(uint32_t index) const {
    const auto &query = state(index).query;
    return query && query->phase != CallQuery::Phase::finished;
}
bool OperationLedger::advances(uint32_t index, ActionStamp stamp) const {
    return before(state(index).latest, stamp);
}

// Bodyless snapshots still use the original Call query validator and ActionRecord invariants.
// Private authorization/input interpretation is deliberately absent, never reconstructed.
bool OperationLedger::valid_record(const OperationRecord &record) {
    const auto &id = record.identity;
    const auto &state = record.state;
    catalog::CallContract normalized{};
    if (!foundation::is_kernel_id(id.call_id) || !foundation::is_kernel_id(id.operation_id) ||
        !foundation::is_kernel_id(id.task_id) || !foundation::is_kernel_id(id.owner_instance_id) ||
        id.call_id == id.operation_id || id.binding_id.size() == 0 ||
        id.capability_id.size() == 0 || id.provider_id.size() == 0 || id.endpoint.size() == 0 ||
        id.binding_version == 0 || id.capability_version == 0 || id.provider_version == 0 ||
        record.contract.is_ephemeral() ||
        catalog::normalize_call_contract(record.contract, normalized) != AYNI_OK ||
        !catalog::same_call_contract(normalized, record.contract) ||
        !ActionRecord::valid(record.action) || !ActionRecord::unresolved(record.action) ||
        record.action.identity.logical_call_id != id.call_id ||
        record.action.identity.operation_id != id.operation_id ||
        record.action.identity.binding_version != id.binding_version ||
        !record.action.identity.binding_id.equals({id.binding_id.data(), id.binding_id.size()}) ||
        !before(state.created, state.archived) ||
        !(same_stamp(state.archived, state.latest) || before(state.archived, state.latest)) ||
        !before(state.created, record.action.prepared) ||
        !before(record.action.latest, state.archived) || !valid_stop(state.execution_stop) ||
        ((record.contract.effects & catalog::call_effect_physical) != 0) !=
            state.execution_stop.has_value() ||
        ((record.contract.effects & catalog::call_effect_egress) != 0) !=
            (id.destination.size() != 0) ||
        state.created.transition.commit_seq == 0 || state.created.transition.transition_id == 0 ||
        foundation::id_boot_epoch(id.call_id) != state.created.boot_epoch ||
        foundation::id_boot_epoch(id.task_id) > state.created.boot_epoch ||
        foundation::id_boot_epoch(id.owner_instance_id) > state.created.boot_epoch ||
        state.attempt.boot_epoch < state.created.boot_epoch ||
        state.latest.boot_epoch > foundation::max_boot_epoch || id.call_id == id.task_id ||
        id.call_id == id.owner_instance_id || id.operation_id == id.task_id ||
        id.operation_id == id.owner_instance_id || record.contract.business_contract.size == 0 ||
        state.attempt.request_id == 0 || state.attempt.boot_epoch == 0 ||
        state.attempt.boot_epoch > state.archived.boot_epoch || state.revision == 0 ||
        (record.contract.business_contract.size != 0 && !record.contract.business_contract.data)) {
        return false;
    }
    CallRecord call{};
    call.contract = record.contract;
    call.action = record.action;
    call.state.created = state.created;
    call.state.latest = state.latest;
    call.state.attempt = state.attempt;
    call.state.outcome = state.outcome;
    call.state.status = state.status;
    call.state.reason = state.reason;
    call.state.query = state.query;
    return valid_call_outcome(call) && CallTable::valid_query(call) &&
           state.revision == query_revision(state.query);
}

ayni_status_t OperationLedger::check_admit_from_call(const CallIdentity &id,
                                                     const catalog::CallContract &contract,
                                                     const CallState &state,
                                                     const ActionFact &action,
                                                     ActionStamp stamp) const {
    if (!has_room(1) || contract.business_contract.size > limits_.business_bytes) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    if (state.ephemeral || state.outcome == 0 || !ActionRecord::unresolved(action) ||
        action.identity.operation_id !=
            (state.subscription ? state.subscription->generation : id.operation_id) ||
        (state.query && state.query->phase != CallQuery::Phase::finished) ||
        (state.execution && !execution_stopped(*state.execution)) || !before(state.latest, stamp) ||
        contains_identity(id.call_id) || contains_identity(action.identity.operation_id) ||
        !valid_record(from_call(id, contract, state, action, stamp))) {
        return AYNI_INVALID_STATE;
    }
    return AYNI_OK;
}
uint32_t OperationLedger::empty_slot() const {
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        if (!occupied(at)) {
            return at;
        }
    }
    return no_index;
}
void OperationLedger::install(uint32_t index, const OperationRecord &record) {
    auto &entry = slots_[index].entry.emplace();
    entry.identity = record.identity;
    entry.contract = record.contract;
    entry.state = record.state;
    entry.action.restore_validated(record.action);
    if (record.contract.business_contract.size) {
        std::memcpy(proof(index), record.contract.business_contract.data,
                    record.contract.business_contract.size);
    }
    entry.contract.business_contract = {proof(index), record.contract.business_contract.size};
    ++size_;
}
void OperationLedger::admit_checked_call(const CallIdentity &id,
                                         const catalog::CallContract &contract,
                                         const CallState &state, const ActionFact &action,
                                         ActionStamp stamp) {
    install(empty_slot(), from_call(id, contract, state, action, stamp));
}
ayni_status_t OperationLedger::check_query(uint32_t index, uint32_t revision) const {
    if (state(index).revision != revision) {
        return AYNI_REVISION_CONFLICT;
    }
    if (query_pending(index)) {
        return AYNI_INVALID_STATE;
    }
    if (contract(index).query == catalog::QueryAuthority::none) {
        return AYNI_UNSUPPORTED;
    }
    return revision == UINT32_MAX ? AYNI_CAPACITY_EXCEEDED : AYNI_OK;
}
void OperationLedger::changed(uint32_t index, OperationChange::Kind kind, ActionStamp stamp) {
    auto &entry = *slots_[index].entry;
    entry.state.latest = stamp;
    const auto query = entry.state.query.value_or(CallQuery{});
    journal_change_ = OperationChange{entry.identity.call_id,
                                      kind,
                                      entry.state.revision,
                                      query.generation,
                                      entry.state.outcome,
                                      query.status,
                                      query.reason,
                                      entry.action.fact().effect,
                                      query.conclusion,
                                      query.attempt,
                                      stamp};
}
ayni_status_t OperationLedger::request_query(uint32_t index, ActionStamp stamp) {
    const auto status = check_query(index, state(index).revision);
    if (status != AYNI_OK || !advances(index, stamp)) {
        return status == AYNI_OK ? AYNI_INVALID_STATE : status;
    }
    auto &state = slots_[index].entry->state;
    state.query = CallQuery{state.revision++, CallQuery::Phase::requested, stamp};
    changed(index, OperationChange::Kind::query_requested, stamp);
    return AYNI_OK;
}
ayni_status_t OperationLedger::begin_query(uint32_t index, ayni_attempt_ref_v1 attempt,
                                           ActionStamp stamp) {
    auto &query = slots_[index].entry->state.query;
    if (!query || query->phase != CallQuery::Phase::requested || !advances(index, stamp) ||
        attempt.request_id == 0 || attempt.boot_epoch != stamp.boot_epoch ||
        foundation::same_attempt(attempt, state(index).attempt)) {
        return AYNI_INVALID_STATE;
    }
    query->phase = CallQuery::Phase::dispatching;
    query->attempt = attempt;
    changed(index, OperationChange::Kind::query_dispatching, stamp);
    return AYNI_OK;
}
ayni_status_t OperationLedger::end_query(uint32_t index, ayni_status_t status, ayni_reason_t reason,
                                         ActionStamp stamp) {
    if (!query_pending(index) || status >= AYNI_OK || !advances(index, stamp)) {
        return AYNI_INVALID_STATE;
    }
    auto &query = *slots_[index].entry->state.query;
    query.phase = CallQuery::Phase::finished;
    query.status = status;
    query.reason = reason;
    query.conclusion = AYNI_QUERY_RESULT_UNKNOWN;
    changed(index, OperationChange::Kind::query_failed, stamp);
    return AYNI_OK;
}

// Reuse ActionRecord's exact authority/effect transition, with an ended original attempt.
// No result bytes are retained or published, and there is no path back to execution.
ayni_status_t OperationLedger::reconcile_and_remove(uint32_t index, uint32_t conclusion,
                                                    const CallResult &result, ActionStamp stamp) {
    if (!query_pending(index) || state(index).query->phase != CallQuery::Phase::dispatching ||
        !advances(index, stamp)) {
        return AYNI_INVALID_STATE;
    }
    if (conclusion == AYNI_QUERY_RESULT_UNKNOWN ||
        contract(index).query != catalog::QueryAuthority::authoritative) {
        return end_query(index, AYNI_RESULT_UNKNOWN, AYNI_REASON_NONE, stamp);
    }
    if (audit_queue_full()) {
        return AYNI_BUSY;
    }
    if ((conclusion != AYNI_QUERY_RESULT_FOUND && conclusion != AYNI_QUERY_RESULT_NOT_ACCEPTED) ||
        (result.outcome != AYNI_CALL_COMPLETED && result.outcome != AYNI_CALL_FAILED) ||
        result.effect < ActionEffect::none || result.effect >= ActionEffect::unknown ||
        (result.outcome == AYNI_CALL_COMPLETED
             ? result.status != AYNI_OK || result.reason != AYNI_REASON_NONE
             : result.status >= AYNI_OK || result.status == AYNI_RESULT_UNKNOWN) ||
        (conclusion == AYNI_QUERY_RESULT_NOT_ACCEPTED &&
         (result.outcome != AYNI_CALL_FAILED || result.effect != ActionEffect::none))) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &entry = *slots_[index].entry;
    const auto evidence =
        conclusion == AYNI_QUERY_RESULT_NOT_ACCEPTED ? ActionRecord::QueryConclusion::not_accepted
        : result.outcome == AYNI_CALL_COMPLETED      ? ActionRecord::QueryConclusion::completed
                                                     : ActionRecord::QueryConclusion::failed;
    const auto status =
        entry.action.reconcile(evidence, result.effect, ActionRecord::OldAttempt::drained, stamp);
    if (status != AYNI_OK) {
        return status;
    }
    entry.state.outcome = result.outcome;
    entry.state.status = result.status;
    entry.state.reason = result.reason;
    entry.state.query->conclusion = conclusion;
    entry.state.query->status = AYNI_OK;
    entry.state.query->reason = AYNI_REASON_NONE;
    changed(index, OperationChange::Kind::resolved, stamp);
    journal_change_->status = result.status;
    journal_change_->reason = result.reason;
    remove(index, false, stamp);
    return AYNI_OK;
}
ayni_status_t OperationLedger::check_acknowledge(uint32_t index, uint32_t revision) const {
    if (state(index).revision != revision) {
        return AYNI_REVISION_CONFLICT;
    }
    if (query_pending(index)) {
        return AYNI_INVALID_STATE;
    }
    return audit_queue_full() ? AYNI_BUSY : AYNI_OK;
}
ayni_status_t OperationLedger::acknowledge_and_remove(uint32_t index, ActionStamp stamp) {
    const auto status = check_acknowledge(index, state(index).revision);
    if (status != AYNI_OK || !advances(index, stamp)) {
        return status == AYNI_OK ? AYNI_INVALID_STATE : status;
    }
    changed(index, OperationChange::Kind::acknowledged, stamp);
    journal_change_->status = state(index).status;
    journal_change_->reason = state(index).reason;
    remove(index, true, stamp);
    return AYNI_OK;
}
void OperationLedger::remove(uint32_t index, bool acknowledged, ActionStamp stamp) {
    const auto &entry = *slots_[index].entry;
    audits_[(audit_head_ + audit_count_++) % limits_.capacity] = {
        entry.identity.call_id,
        entry.identity.operation_id,
        entry.identity.binding_id,
        entry.state.outcome,
        entry.state.status,
        entry.state.reason,
        acknowledged,
        (entry.contract.effects & catalog::call_effect_physical) != 0,
        stamp.transition.commit_seq};
    std::memset(proof(index), 0, limits_.business_bytes);
    slots_[index].entry.reset();
    --size_;
}
bool OperationLedger::has_durable_audit(uint64_t durable_seq) const {
    return audit_count_ && audits_[audit_head_].after_seq <= durable_seq;
}
bool OperationLedger::take_durable_audit(uint64_t durable_seq, PendingAudit &out) {
    if (!has_durable_audit(durable_seq)) {
        return false;
    }
    out = audits_[audit_head_];
    audits_[audit_head_] = {};
    audit_head_ = (audit_head_ + 1U) % limits_.capacity;
    --audit_count_;
    return true;
}
ayni_status_t OperationLedger::restore_snapshot_entry(const OperationRecord &record) {
    if (!valid_record(record) || contains_identity(record.identity.call_id) ||
        contains_identity(record.identity.operation_id)) {
        return AYNI_INVALID_STATE;
    }
    if (!has_room(1) || record.contract.business_contract.size > limits_.business_bytes) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    install(empty_slot(), record);
    return AYNI_OK;
}
void OperationLedger::clear() {
    for (uint32_t at = 0; at < limits_.capacity; ++at) {
        slots_[at].entry.reset();
        std::memset(proof(at), 0, limits_.business_bytes);
        audits_[at] = {};
    }
    size_ = audit_head_ = audit_count_ = 0;
    journal_change_.reset();
}
} // namespace ayni::work

namespace ayni::work {
namespace {
bool same_change(const OperationChange &a, const OperationChange &b) {
    return a.call_id == b.call_id && a.kind == b.kind && a.revision == b.revision &&
           a.generation == b.generation && a.outcome == b.outcome && a.status == b.status &&
           a.reason == b.reason && a.effect == b.effect && a.conclusion == b.conclusion &&
           foundation::same_attempt(a.attempt, b.attempt) && same_stamp(a.stamp, b.stamp);
}
} // namespace
// Replay only accepts the exact canonical delta generated by the same live transition rules.
// These commits already belong to the durable prefix; do not re-emit historical RAM audits.
ayni_status_t OperationLedger::restore_journal_change(const OperationChange &change) {
    uint32_t index = 0;
    if (!find(change.call_id, index)) {
        return AYNI_INVALID_STATE;
    }
    ayni_status_t status = AYNI_INVALID_STATE;
    switch (change.kind) {
    case OperationChange::Kind::query_requested:
        status = request_query(index, change.stamp);
        break;
    case OperationChange::Kind::query_dispatching:
        status = begin_query(index, change.attempt, change.stamp);
        break;
    case OperationChange::Kind::query_failed:
        status = end_query(index, change.status, change.reason, change.stamp);
        break;
    case OperationChange::Kind::resolved:
        status = reconcile_and_remove(
            index, change.conclusion,
            {change.outcome, change.status, change.reason, change.effect, {}}, change.stamp);
        break;
    case OperationChange::Kind::acknowledged:
        status = acknowledge_and_remove(index, change.stamp);
        break;
    }
    if (status != AYNI_OK || !journal_change_ || !same_change(*journal_change_, change)) {
        return AYNI_INVALID_STATE;
    }
    PendingAudit ignored{};
    while (take_durable_audit(UINT64_MAX, ignored)) {
    }
    return AYNI_OK;
}
} // namespace ayni::work
