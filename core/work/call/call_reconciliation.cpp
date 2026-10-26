#include "work/call/call_table.h"

#include <cstring>

namespace ayni::work {
uint32_t CallTable::query_revision(uint32_t index) const {
    const auto &query = state(index).query;
    return query ? query->generation + initial_query_revision : initial_query_revision;
}

bool CallTable::unresolved(uint32_t index) const {
    const auto *fact = action(index);
    return fact != nullptr && ActionRecord::unresolved(*fact);
}

bool CallTable::query_pending(uint32_t index) const {
    const auto &query = state(index).query;
    return query && query->phase != CallQuery::Phase::finished;
}

ayni_status_t CallTable::check_query(uint32_t index, uint32_t expected_revision) const {
    if (expected_revision != query_revision(index)) {
        return AYNI_REVISION_CONFLICT;
    }
    if (!unresolved(index) || query_pending(index)) {
        return AYNI_INVALID_STATE;
    }
    if (contract(index).query == catalog::QueryAuthority::none) {
        return AYNI_UNSUPPORTED;
    }
    return expected_revision == UINT32_MAX ? AYNI_CAPACITY_EXCEEDED : AYNI_OK;
}

ayni_status_t CallTable::request_query(uint32_t index, ActionStamp stamp) {
    auto &entry = *slots_[index].entry;
    const auto status = check_query(index, query_revision(index));
    if (status != AYNI_OK || !advances(entry, stamp)) {
        return status == AYNI_OK ? AYNI_INVALID_STATE : status;
    }
    const uint32_t generation = query_revision(index);
    entry.state.query = CallQuery{generation, CallQuery::Phase::requested, stamp};
    entry.state.latest = stamp;
    return AYNI_OK;
}

ayni_status_t CallTable::begin_query(uint32_t index, ayni_attempt_ref_v1 attempt,
                                     ActionStamp stamp) {
    auto &entry = *slots_[index].entry;
    auto &query = entry.state.query;
    if (!query || query->phase != CallQuery::Phase::requested || !advances(entry, stamp) ||
        attempt.boot_epoch != stamp.boot_epoch || attempt.request_id == 0U ||
        (attempt.boot_epoch == entry.state.attempt.boot_epoch &&
         attempt.request_id == entry.state.attempt.request_id)) {
        return AYNI_INVALID_STATE;
    }
    query->attempt = attempt;
    query->phase = CallQuery::Phase::dispatching;
    entry.state.latest = stamp;
    return AYNI_OK;
}

ayni_status_t CallTable::end_query(uint32_t index, ayni_status_t status, ayni_reason_t reason,
                                   ActionStamp stamp) {
    auto &entry = *slots_[index].entry;
    if (!query_pending(index) || !advances(entry, stamp) || status >= AYNI_OK) {
        return AYNI_INVALID_STATE;
    }
    auto &query = *entry.state.query;
    query.phase = CallQuery::Phase::finished;
    query.status = status;
    query.reason = reason;
    query.conclusion = AYNI_QUERY_RESULT_UNKNOWN;
    entry.state.latest = stamp;
    return AYNI_OK;
}

// C-LIFE-010: only the original Provider's authoritative query can resolve an
// execution. Weak FOUND and missing-record results both retain UNKNOWN; neither
// grants authority to retry the original operation.
ayni_status_t CallTable::reconcile(uint32_t index, uint32_t conclusion, const CallResult &input,
                                   ActionStamp stamp, ResultDependencies dependencies) {
    auto &entry = *slots_[index].entry;
    const auto &query = entry.state.query;
    if (!query || query->phase != CallQuery::Phase::dispatching || !advances(entry, stamp) ||
        !unresolved(index) || !entry.action) {
        return AYNI_INVALID_STATE;
    }
    if (conclusion == AYNI_QUERY_RESULT_UNKNOWN ||
        entry.contract.query != catalog::QueryAuthority::authoritative) {
        return end_query(index, AYNI_RESULT_UNKNOWN, AYNI_REASON_NONE, stamp);
    }
    const auto checked = check_result(index, input);
    if (checked != AYNI_OK) {
        return checked;
    }
    if ((input.outcome != AYNI_CALL_COMPLETED && input.outcome != AYNI_CALL_FAILED) ||
        (conclusion != AYNI_QUERY_RESULT_FOUND && conclusion != AYNI_QUERY_RESULT_NOT_ACCEPTED) ||
        (conclusion == AYNI_QUERY_RESULT_NOT_ACCEPTED &&
         (input.outcome != AYNI_CALL_FAILED || input.effect != ActionEffect::none ||
          input.text.size != 0U))) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto result = usable_result(input, dependencies);
    if (entry.state.content_erased) {
        result.text = {};
        result.kind = CallResultKind::text;
    }

    const auto fact =
        conclusion == AYNI_QUERY_RESULT_NOT_ACCEPTED ? ActionRecord::QueryConclusion::not_accepted
        : result.outcome == AYNI_CALL_COMPLETED      ? ActionRecord::QueryConclusion::completed
                                                     : ActionRecord::QueryConclusion::failed;
    const auto status =
        entry.action->reconcile(fact, result.effect, ActionRecord::OldAttempt::drained, stamp);
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
    entry.state.query->phase = CallQuery::Phase::finished;
    entry.state.query->conclusion = conclusion;
    return AYNI_OK;
}
} // namespace ayni::work
