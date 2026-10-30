// White-box, C-SUB-002/003 / ADR-0063: finite lease facts retain monotone
// collection intent and require known closure before opening another generation.
#include "foundation/kernel_id.h"
#include "work/subscription/subscription_fact.h"

#include <gtest/gtest.h>
#include <iterator>

namespace {
using namespace ayni::work;
SubscriptionFact lease_fact(SubscriptionPhase phase) {
    SubscriptionFact fact{};
    fact.phase = phase;
    fact.lease_sequence = phase == SubscriptionPhase::starting ? 0 : 1;
    ayni::foundation::IdAllocator ids;
    ids.start(1);
    EXPECT_TRUE(ids.allocate(fact.generation));
    return fact;
}

TEST(SubscriptionFacts, legal_same_generation_phases_follow_the_declared_transition_matrix) {
    constexpr SubscriptionPhase phases[] = {SubscriptionPhase::starting, SubscriptionPhase::active,
                                            SubscriptionPhase::stopping, SubscriptionPhase::stopped,
                                            SubscriptionPhase::expired,  SubscriptionPhase::failed};
    // STARTING, ACTIVE, STOPPING, STOPPED, EXPIRED, FAILED. Lease confirmation
    // grows by one; a known terminal cannot regain an active/starting phase.
    constexpr bool allowed[][6] = {
        {true, true, true, true, true, true},    {false, true, true, true, true, true},
        {false, false, true, true, false, true}, {false, false, false, true, false, false},
        {false, false, false, true, true, true}, {false, false, false, true, false, true}};
    for (std::size_t from = 0; from < std::size(phases); ++from) {
        const auto before = lease_fact(phases[from]);
        ASSERT_TRUE(valid_subscription(before));
        for (std::size_t to = 0; to < std::size(phases); ++to) {
            auto after = lease_fact(phases[to]);
            ASSERT_TRUE(valid_subscription(after));
            EXPECT_EQ(subscription_advances(before, after), allowed[from][to]) << from << ',' << to;
        }
    }
}

TEST(SubscriptionFacts, invalid_lease_fields_are_rejected_before_any_transition) {
    const auto original = lease_fact(SubscriptionPhase::active);
    const auto changes = {
        +[](SubscriptionFact &f) { f.generation = 0; },
        +[](SubscriptionFact &f) { f.generation = 1; },
        +[](SubscriptionFact &f) { f.control_status = 1; },
        +[](SubscriptionFact &f) { f.control_status = AYNI_INTERNAL - 1; },
        +[](SubscriptionFact &f) { f.control_reason = AYNI_REASON_PROVIDER_RESULT; },
        +[](SubscriptionFact &f) { f.phase = static_cast<SubscriptionPhase>(0); },
        +[](SubscriptionFact &f) { f.phase = static_cast<SubscriptionPhase>(UINT32_MAX); },
        +[](SubscriptionFact &f) { f.desired = static_cast<SubscriptionDesired>(0); },
        +[](SubscriptionFact &f) { f.lease_sequence = 0; },
        +[](SubscriptionFact &f) { f.phase = SubscriptionPhase::starting; },
        +[](SubscriptionFact &f) { f.control_status = AYNI_NOT_READY; },
    };
    for (const auto change : changes) {
        auto candidate = original;
        change(candidate);
        EXPECT_FALSE(valid_subscription(candidate));
        EXPECT_FALSE(subscription_advances(original, candidate));
    }
}

TEST(SubscriptionFacts, new_generation_requires_open_intent_and_confirmed_stop) {
    auto stopped = lease_fact(SubscriptionPhase::stopped);
    ++stopped.generation;
    auto next = lease_fact(SubscriptionPhase::starting);
    next.generation = stopped.generation + 1;
    ASSERT_TRUE(valid_subscription(next));
    EXPECT_TRUE(subscription_advances(stopped, next));
    for (const auto phase :
         {SubscriptionPhase::starting, SubscriptionPhase::active, SubscriptionPhase::stopping,
          SubscriptionPhase::expired, SubscriptionPhase::failed}) {
        auto before = lease_fact(phase);
        EXPECT_FALSE(subscription_advances(before, next));
    }
    auto changed = stopped;
    changed.desired = SubscriptionDesired::closed;
    EXPECT_FALSE(subscription_advances(changed, next));
    changed = next;
    --changed.generation;
    --changed.generation;
    EXPECT_FALSE(subscription_advances(stopped, changed));
    changed = next;
    changed.phase = SubscriptionPhase::active;
    changed.lease_sequence = 1;
    EXPECT_FALSE(subscription_advances(stopped, changed));
}

TEST(SubscriptionFacts, acknowledgement_and_closed_intent_cannot_regress_or_skip) {
    auto active = lease_fact(SubscriptionPhase::active);
    auto next = active;
    ++next.lease_sequence;
    EXPECT_TRUE(subscription_advances(active, next));
    ++next.lease_sequence;
    EXPECT_FALSE(subscription_advances(active, next));
    next = active;
    --next.lease_sequence;
    next.phase = SubscriptionPhase::stopping;
    ASSERT_TRUE(valid_subscription(next));
    EXPECT_FALSE(subscription_advances(active, next));
    active.lease_sequence = UINT64_MAX;
    next = active;
    next.phase = SubscriptionPhase::stopped;
    EXPECT_TRUE(subscription_advances(active, next));
    next.lease_sequence = 0;
    EXPECT_FALSE(subscription_advances(active, next));
    active = lease_fact(SubscriptionPhase::active);
    active.desired = SubscriptionDesired::closed;
    next = active;
    next.desired = SubscriptionDesired::open;
    EXPECT_FALSE(subscription_advances(active, next));
}

TEST(SubscriptionFacts, failed_control_diagnostic_is_immutable_through_known_stop) {
    auto failed = lease_fact(SubscriptionPhase::failed);
    failed.desired = SubscriptionDesired::closed;
    failed.control_status = AYNI_RESULT_UNKNOWN;
    failed.control_reason = AYNI_REASON_PROVIDER_RESULT;
    ASSERT_TRUE(valid_subscription(failed));
    auto next = failed;
    next.control_status = AYNI_NOT_READY;
    next.control_reason = AYNI_REASON_NONE;
    ASSERT_TRUE(valid_subscription(next));
    EXPECT_FALSE(subscription_advances(failed, next));
    next = failed;
    next.control_reason = AYNI_REASON_NONE;
    ASSERT_TRUE(valid_subscription(next));
    EXPECT_FALSE(subscription_advances(failed, next));
    next = failed;
    next.phase = SubscriptionPhase::stopped;
    // Intrinsic control-error shape requires FAILED until its owner consumes it.
    EXPECT_FALSE(valid_subscription(next));
    next = lease_fact(SubscriptionPhase::failed);
    next.desired = SubscriptionDesired::closed;
    next.control_status = AYNI_NOT_READY;
    next.control_reason = AYNI_REASON_PROVIDER_RESULT;
    EXPECT_FALSE(valid_subscription(next));
}
} // namespace
