#include "foundation/attempt_identity.h"
#include "work/call/call_table.h"

namespace ayni::work {
namespace {
bool same_stamp(ActionStamp left, ActionStamp right) {
    return left.boot_epoch == right.boot_epoch &&
           left.transition.commit_seq == right.transition.commit_seq &&
           left.transition.transition_id == right.transition.transition_id;
}
} // namespace

// Query state cannot exist before an execution has become UNKNOWN. Every
// finished positive conclusion must agree with the retained execution ledger.
bool CallTable::valid_query(const CallRecord &record) {
    const auto &state = record.state;
    if (!state.query) {
        return true;
    }
    const auto &query = *state.query;
    const bool unresolved = record.action && ActionRecord::unresolved(*record.action);
    if (query.generation == 0U || query.generation == UINT32_MAX ||
        record.contract.query == catalog::QueryAuthority::none ||
        query.requested.boot_epoch < state.created.boot_epoch ||
        query.requested.boot_epoch > state.latest.boot_epoch ||
        query.requested.transition.commit_seq <= state.created.transition.commit_seq ||
        query.requested.transition.transition_id <= state.created.transition.transition_id ||
        query.requested.transition.commit_seq > state.latest.transition.commit_seq ||
        query.requested.transition.transition_id > state.latest.transition.transition_id ||
        (query.attempt.boot_epoch == 0U) != (query.attempt.request_id == 0U) ||
        (query.attempt.request_id != 0U &&
         (query.attempt.boot_epoch < query.requested.boot_epoch ||
          query.attempt.boot_epoch > state.latest.boot_epoch ||
          foundation::same_attempt(query.attempt, state.attempt)))) {
        return false;
    }
    switch (query.phase) {
    case CallQuery::Phase::requested:
        return same_stamp(query.requested, state.latest) && query.attempt.request_id == 0U &&
               unresolved && query.status == AYNI_OK && query.reason == AYNI_REASON_NONE &&
               query.conclusion == 0U;
    case CallQuery::Phase::dispatching:
        return query.attempt.request_id != 0U && unresolved && query.status == AYNI_OK &&
               query.reason == AYNI_REASON_NONE && query.conclusion == 0U;
    case CallQuery::Phase::finished:
        if (query.conclusion == AYNI_QUERY_RESULT_UNKNOWN) {
            return query.status < AYNI_OK && unresolved;
        }
        return record.contract.query == catalog::QueryAuthority::authoritative &&
               query.attempt.request_id != 0U && query.status == AYNI_OK &&
               query.reason == AYNI_REASON_NONE &&
               (state.outcome == AYNI_CALL_COMPLETED || state.outcome == AYNI_CALL_FAILED) &&
               (query.conclusion == AYNI_QUERY_RESULT_FOUND ||
                (query.conclusion == AYNI_QUERY_RESULT_NOT_ACCEPTED && record.action &&
                 record.contract.query == catalog::QueryAuthority::authoritative &&
                 record.action->evidence == ActionEvidence::authoritative_not_accepted));
    }
    return false;
}

// A terminal execution changes only through an explicitly accepted query.
// Same-generation delivery never changes its command stamp or allocated attempt.
bool CallTable::query_advances(const CallState &before, const CallState &after) {
    if (before.outcome == 0U) {
        return !after.query;
    }
    if (!after.query || before.progress_sequence != after.progress_sequence ||
        before.cancel_cause != after.cancel_cause || before.cancel_result != after.cancel_result) {
        return false;
    }
    const auto &next = *after.query;
    if (!before.query || before.query->phase == CallQuery::Phase::finished) {
        const uint32_t generation =
            before.query ? before.query->generation + 1U : initial_query_revision;
        return next.generation == generation && next.phase == CallQuery::Phase::requested;
    }
    const auto &old = *before.query;
    return old.generation == next.generation && same_stamp(old.requested, next.requested) &&
           (old.attempt.request_id == 0U || foundation::same_attempt(old.attempt, next.attempt)) &&
           ((old.phase == CallQuery::Phase::requested &&
             next.phase == CallQuery::Phase::dispatching) ||
            next.phase == CallQuery::Phase::finished);
}
} // namespace ayni::work
