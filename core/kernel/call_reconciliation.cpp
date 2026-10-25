#include "kernel/call_runtime.h"

#include "foundation/deadline.h"

namespace ayni::kernel {
namespace {
constexpr uint64_t microseconds_per_millisecond = 1000;
ayni_string_v1 view(const catalog::CatalogId &id) {
    return {id.data(), id.size()};
}
} // namespace

CallRuntime::Advance CallRuntime::end_query(uint32_t index, ayni_status_t status,
                                            ayni_reason_t reason) {
    return work_.calls.end_query(index, status, reason,
                                 {work_.ids.boot_epoch(), work_.transitions.next()}) == AYNI_OK
               ? Advance::applied
               : Advance::internal;
}

// Query has its own transport attempt and durable barrier. It only reads the
// original operation; it consumes no execution permit or Task model-call count.
CallRuntime::Advance CallRuntime::prepare_query(uint32_t index) {
    if (mailbox_.in_flight() == limits_.capacity) {
        return Advance::idle;
    }
    const auto &id = work_.calls.identity(index);
    uint32_t provider_index = 0;
    if (!catalogs_.providers.find(view(id.provider_id), id.provider_version, provider_index)) {
        return end_query(index, AYNI_UNSUPPORTED_VERSION, AYNI_REASON_NONE);
    }
    const auto &provider = catalogs_.providers.at(provider_index);
    if (provider.query == nullptr ||
        !catalog::ProviderRegistry::matches(provider, view(id.capability_id), id.capability_version,
                                            work_.calls.contract(index), view(id.endpoint),
                                            view(id.destination))) {
        return end_query(index, AYNI_UNSUPPORTED_VERSION, AYNI_REASON_NONE);
    }
    ayni_attempt_ref_v1 attempt{};
    ProviderReservation reservation{};
    if (!attempts_.next(work_.ids.boot_epoch(), attempt)) {
        return Advance::attempt_exhausted;
    }
    if (mailbox_.reserve(attempt, reservation) != AYNI_OK) {
        return Advance::internal;
    }
    if (work_.calls.begin_query(index, attempt,
                                {work_.ids.boot_epoch(), work_.transitions.next()}) != AYNI_OK) {
        static_cast<void>(mailbox_.withdraw(reservation));
        return Advance::internal;
    }
    slots_[index].active = Active{id.call_id,
                                  Stage::barrier,
                                  {},
                                  Delivery{reservation, &provider, UINT64_MAX,
                                           Delivery::Cancel::none, Delivery::Purpose::query}};
    return Advance::applied;
}

CallRuntime::Advance CallRuntime::dispatch_query(uint32_t index, uint64_t now_us) {
    auto &active = *slots_[index].active;
    auto &delivery = *active.delivery;
    const auto &id = work_.calls.identity(index);
    delivery.deadline_us = foundation::deadline(
        now_us, uint64_t{work_.calls.contract(index).timeout_ms} * microseconds_per_millisecond);
    active.stage = Stage::submitted;
    work_.calling_out = true;
    const auto submitted = delivery.provider->query(delivery.provider->context, id.operation_id,
                                                    delivery.reservation.attempt,
                                                    mailbox_.completion(delivery.reservation));
    work_.calling_out = false;
    if (submitted == AYNI_SUBMIT_REJECTED_NOT_RETAINED && mailbox_.withdraw(delivery.reservation)) {
        const auto result = end_query(index, AYNI_NOT_READY, AYNI_REASON_NONE);
        release(index, now_us);
        return result;
    }
    return submitted == AYNI_SUBMIT_ACCEPTED || submitted == AYNI_SUBMIT_ACCEPT_UNKNOWN ||
                   submitted == AYNI_SUBMIT_REJECTED_NOT_RETAINED
               ? Advance::applied
               : Advance::internal;
}

// A restored query that may already have been sent is not silently repeated.
// Its execution remains UNKNOWN and a new owner command can request another read.
CallRuntime::Advance CallRuntime::advance_query(uint32_t index, uint64_t now_us,
                                                uint64_t durable_seq, Mode mode) {
    const auto &query = *work_.calls.state(index).query;
    auto &active = slots_[index].active;
    if (!active) {
        if (mode != Mode::running || query.phase == work::CallQuery::Phase::dispatching) {
            return end_query(index, AYNI_RESULT_UNKNOWN, AYNI_REASON_NONE);
        }
        return work_.calls.state(index).latest.transition.commit_seq <= durable_seq
                   ? prepare_query(index)
                   : Advance::idle;
    }
    if (mode != Mode::running || now_us >= active->delivery->deadline_us) {
        if (active->stage == Stage::submitted) {
            return cancel_transport(*active->delivery);
        }
        if (!mailbox_.withdraw(active->delivery->reservation)) {
            return Advance::internal;
        }
        const auto result = end_query(index, AYNI_RESULT_UNKNOWN, AYNI_REASON_NONE);
        release(index, now_us);
        return result;
    }
    return active->stage == Stage::barrier &&
                   work_.calls.state(index).latest.transition.commit_seq <= durable_seq
               ? dispatch_query(index, now_us)
               : Advance::idle;
}

bool CallRuntime::query_ready(uint32_t index, uint64_t now_us, uint64_t durable_seq,
                              Mode mode) const {
    const auto &active = slots_[index].active;
    if (!active) {
        return mode != Mode::running ||
               work_.calls.state(index).query->phase == work::CallQuery::Phase::dispatching ||
               (work_.calls.state(index).latest.transition.commit_seq <= durable_seq &&
                mailbox_.in_flight() < limits_.capacity);
    }
    if (mode != Mode::running || now_us >= active->delivery->deadline_us) {
        return active->stage != Stage::submitted ||
               active->delivery->cancel == Delivery::Cancel::none;
    }
    return active->stage == Stage::barrier &&
           work_.calls.state(index).latest.transition.commit_seq <= durable_seq;
}

// Query failures affect query diagnostics alone. A successful conclusion joins
// the original Task only if it still waits for that call; cancellation is final.
bool CallRuntime::consume_query(uint32_t index, uint32_t task_index, const ProviderMessage &message,
                                foundation::TransitionStamp stamp) {
    const auto kind = message.kind == ProviderMessageKind::completed ? AYNI_CALL_COMPLETED
                      : message.kind == ProviderMessageKind::failed  ? AYNI_CALL_FAILED
                                                                     : AYNI_CALL_ABANDONED;
    work::CallResult result{
        kind,
        message.status,
        message.reason,
        static_cast<work::ActionEffect>(message.effect),
        {reinterpret_cast<const char *>(message.payload.data), message.payload.size}};
    const work::ActionStamp action_stamp{work_.ids.boot_epoch(), stamp};
    auto status = work_.calls.reconcile(index, message.query_result, result, action_stamp,
                                        result_dependencies(index));
    if (status == AYNI_INVALID_ARGUMENT || status == AYNI_CAPACITY_EXCEEDED) {
        status = work_.calls.end_query(index, AYNI_RESULT_UNKNOWN, AYNI_REASON_PROVIDER_RESULT,
                                       action_stamp);
    }
    if (status != AYNI_OK) {
        return false;
    }
    return work_.calls.state(index).outcome == AYNI_CALL_ABANDONED ||
           work_.tasks.at(task_index).call.id != work_.calls.identity(index).call_id ||
           finish_task(index, task_index, stamp);
}
} // namespace ayni::kernel
