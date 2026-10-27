// One Task's durable latest-observation/state frontier, never a frame history.
#ifndef AYNI_CORE_WORK_SUBSCRIPTION_WAIT_H
#define AYNI_CORE_WORK_SUBSCRIPTION_WAIT_H
#include "foundation/kernel_id.h"
#include "work/subscription/subscription_fact.h"
namespace ayni::work {
struct SubscriptionWait {
    uint64_t subscription_id = 0; // Positive retained Call ID, owned by the waiting Task.
    uint64_t generation = 0;      // Positive collection identity at the decision's frontier.
    uint64_t sequence = 0;        // Last observed sequence; 0 explicitly means no sample yet.
    SubscriptionPhase phase =
        SubscriptionPhase::starting; // State frontier, excluding renew ACK count.
    // Same shape rule for a staged request and record bytes; ownership is cross-table proof.
    bool valid() const {
        return foundation::is_kernel_id(subscription_id) && foundation::is_kernel_id(generation) &&
               phase >= SubscriptionPhase::starting && phase <= SubscriptionPhase::failed;
    }
    // Busy consumers wake against the latest frontier, never a retained per-frame queue.
    bool operator==(const SubscriptionWait &other) const {
        return subscription_id == other.subscription_id && generation == other.generation &&
               sequence == other.sequence && phase == other.phase;
    }
    bool operator!=(const SubscriptionWait &other) const { return !(*this == other); }
};
} // namespace ayni::work
#endif
