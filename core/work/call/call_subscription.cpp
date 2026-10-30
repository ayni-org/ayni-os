#include "foundation/kernel_id.h"
#include "work/call/call_table.h"
namespace ayni::work {
// Lease confirmation is independent of optional progress sequence numbers.
// The first ACK changes attachment in the same transition as Task detach.
ayni_status_t CallTable::confirm_subscription(uint32_t index, uint64_t sequence,
                                              ActionStamp stamp) {
    auto &entry = *slots_[index].entry;
    if (!entry.state.subscription || !pending(entry) || !advances(entry, stamp) || !entry.action ||
        entry.state.cancel_cause != CallCancelCause::none) {
        return AYNI_INVALID_STATE;
    }
    auto &fact = *entry.state.subscription;
    if ((fact.phase != SubscriptionPhase::starting && fact.phase != SubscriptionPhase::active) ||
        fact.desired != SubscriptionDesired::open || fact.lease_sequence == UINT64_MAX ||
        sequence != fact.lease_sequence + 1) {
        return AYNI_INVALID_STATE;
    }
    const auto status = entry.action->progress(stamp);
    if (status != AYNI_OK) {
        return status;
    }
    fact.phase = SubscriptionPhase::active;
    entry.state.attachment = CallAttachment::background;
    fact.lease_sequence = sequence;
    entry.state.latest = stamp;
    return AYNI_OK;
}
ayni_status_t CallTable::stop_subscription(uint32_t index, CallCancelCause cause,
                                           SubscriptionDesired desired, ActionStamp stamp) {
    auto &entry = *slots_[index].entry;
    if (!entry.state.subscription || !advances(entry, stamp) || cause == CallCancelCause::none ||
        cause > CallCancelCause::shutdown ||
        (desired != SubscriptionDesired::open && desired != SubscriptionDesired::closed)) {
        return AYNI_INVALID_STATE;
    }
    auto &fact = *entry.state.subscription;
    if (fact.desired == SubscriptionDesired::closed && desired != fact.desired) {
        return AYNI_INVALID_STATE;
    }
    fact.desired = desired;
    if (pending(entry)) {
        if (entry.state.cancel_cause == CallCancelCause::none) {
            entry.state.cancel_cause = cause;
        }
        fact.phase = fact.control_status != AYNI_OK       ? SubscriptionPhase::failed
                     : cause == CallCancelCause::deadline ? SubscriptionPhase::expired
                                                          : SubscriptionPhase::stopping;
    }
    entry.state.latest = stamp;
    return AYNI_OK;
}
ayni_status_t CallTable::fail_subscription(uint32_t index, ayni_status_t status,
                                           ayni_reason_t reason, ActionStamp stamp) {
    auto &entry = *slots_[index].entry;
    if (!entry.state.subscription || !pending(entry) || !advances(entry, stamp)) {
        return AYNI_INVALID_STATE;
    }
    auto after = *entry.state.subscription;
    after.control_status = status;
    after.control_reason = reason;
    after.phase = SubscriptionPhase::failed;
    after.desired = SubscriptionDesired::closed;
    if (!subscription_advances(*entry.state.subscription, after)) {
        return AYNI_INVALID_ARGUMENT;
    }
    entry.state.subscription = after;
    if (entry.state.cancel_cause == CallCancelCause::none) {
        entry.state.cancel_cause = CallCancelCause::authority;
    }
    entry.state.latest = stamp;
    return AYNI_OK;
}
// A new collection uses an independently allocated operation ID and one-shot
// ActionRecord. The prior known closure remains in the committed journal history.
ayni_status_t CallTable::restart_subscription(uint32_t index, uint64_t generation,
                                              ActionStamp stamp) {
    auto &entry = *slots_[index].entry;
    if (!entry.state.subscription || !advances(entry, stamp) || entry.state.content_erased ||
        entry.state.outcome != AYNI_CALL_CANCELLED || !foundation::is_kernel_id(generation) ||
        foundation::id_boot_epoch(generation) != stamp.boot_epoch) {
        return AYNI_INVALID_STATE;
    }
    auto after = *entry.state.subscription;
    after.generation = generation;
    after.phase = SubscriptionPhase::starting;
    after.lease_sequence = 0;
    if (!subscription_advances(*entry.state.subscription, after)) {
        return AYNI_INVALID_STATE;
    }
    entry.state.subscription = after;
    entry.state.attempt = {};
    entry.state.progress_sequence = 0;
    entry.state.cancel_cause = CallCancelCause::none;
    entry.state.cancel_result = 0;
    entry.state.outcome = 0;
    entry.state.status = AYNI_OK;
    entry.state.reason = AYNI_REASON_NONE;
    entry.state.latest = stamp;
    entry.action.reset();
    discard_result(index);
    return AYNI_OK;
}
} // namespace ayni::work
