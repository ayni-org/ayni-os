#include "work/subscription/subscription_fact.h"
#include "foundation/kernel_id.h"
namespace ayni::work {
bool valid_subscription(const SubscriptionFact &fact) {
    return foundation::is_kernel_id(fact.generation) && fact.control_status <= AYNI_OK &&
           fact.control_status >= AYNI_INTERNAL &&
           (fact.control_status == AYNI_OK
                ? fact.control_reason == AYNI_REASON_NONE
                : fact.phase == SubscriptionPhase::failed &&
                      fact.desired == SubscriptionDesired::closed &&
                      (fact.control_reason == AYNI_REASON_NONE ||
                       (fact.control_status == AYNI_RESULT_UNKNOWN &&
                        fact.control_reason == AYNI_REASON_PROVIDER_RESULT))) &&
           fact.phase >= SubscriptionPhase::starting && fact.phase <= SubscriptionPhase::failed &&
           (fact.desired == SubscriptionDesired::open ||
            fact.desired == SubscriptionDesired::closed) &&
           (fact.phase != SubscriptionPhase::starting || fact.lease_sequence == 0) &&
           (fact.phase != SubscriptionPhase::active || fact.lease_sequence != 0);
}
// Restart is a new finite session after confirmed stop, never an ActionRecord
// RESULT_UNKNOWN retry. Within one generation, terminal phases cannot reopen.
bool subscription_advances(const SubscriptionFact &before, const SubscriptionFact &after) {
    if (!valid_subscription(after) ||
        (before.control_status != AYNI_OK && (before.control_status != after.control_status ||
                                              before.control_reason != after.control_reason)) ||
        after.generation < before.generation ||
        (before.desired == SubscriptionDesired::closed && after.desired != before.desired)) {
        return false;
    }
    if (after.generation != before.generation) {
        return after.generation > before.generation && before.phase == SubscriptionPhase::stopped &&
               before.desired == SubscriptionDesired::open &&
               after.phase == SubscriptionPhase::starting && after.lease_sequence == 0;
    }
    if (after.lease_sequence < before.lease_sequence ||
        (after.lease_sequence != before.lease_sequence &&
         (before.lease_sequence == UINT64_MAX ||
          after.lease_sequence != before.lease_sequence + 1))) {
        return false;
    }
    if (before.phase == SubscriptionPhase::stopped || before.phase == SubscriptionPhase::failed) {
        return (after.phase == before.phase || (before.phase == SubscriptionPhase::failed &&
                                                after.phase == SubscriptionPhase::stopped)) &&
               after.lease_sequence == before.lease_sequence;
    }
    if (before.phase == SubscriptionPhase::stopping || before.phase == SubscriptionPhase::expired) {
        return after.phase == before.phase || after.phase == SubscriptionPhase::stopped ||
               after.phase == SubscriptionPhase::failed;
    }
    return before.phase == SubscriptionPhase::starting ||
           after.phase != SubscriptionPhase::starting;
}
} // namespace ayni::work
