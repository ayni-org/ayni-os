// Registered strategies owned by catalog (ADR-0022): one version of an id at
// a time, and goal types naming them by id, resolved once registration
// closes.

#include "catalog/goal_type/goal_type_registry.h"
#include "catalog/strategy/strategy_registry.h"

#include <ayni/ayni.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using ayni::catalog::GoalType;
using ayni::catalog::GoalTypeRegistry;
using ayni::catalog::Strategy;
using ayni::catalog::StrategyRegistry;

ayni_string_v1 text(const char *value) {
    return {value, static_cast<uint32_t>(std::strlen(value))};
}

ayni_status_t decide_nothing(void *, const ayni_task_snapshot_v1 *, ayni_policy_output_v1 *) {
    return AYNI_UNSUPPORTED;
}

Strategy strategy(const char *id, uint32_t version) {
    Strategy entry{};
    EXPECT_TRUE(entry.id.assign(text(id)));
    entry.version = version;
    entry.decide = &decide_nothing;
    entry.max_intents_per_call = 1;
    return entry;
}

GoalType goal_type(const char *id, const char *strategy_id) {
    GoalType type{};
    EXPECT_TRUE(type.id.assign(text(id)));
    type.version = 1;
    if (strategy_id[0] != '\0') {
        EXPECT_TRUE(type.strategy_id.assign(text(strategy_id)));
    }
    return type;
}

// Storage outlives the registry it backs.
template <typename Registry> class Owned {
  public:
    explicit Owned(uint32_t capacity)
        : storage_(Registry::storage_size(capacity) / sizeof(uint64_t) + 1U),
          registry_(storage_.data(), capacity) {}
    Registry &operator*() { return registry_; }
    Registry *operator->() { return &registry_; }

  private:
    std::vector<uint64_t> storage_;
    Registry registry_;
};

TEST(StrategyRegistry, finds_each_registered_strategy) {
    Owned<StrategyRegistry> registry(2);
    int ctx = 0;
    Strategy first = strategy("plan", 3);
    first.ctx = &ctx;
    first.max_intents_per_call = 4;
    ASSERT_EQ(registry->add(first), StrategyRegistry::AddResult::added);
    ASSERT_EQ(registry->add(strategy("watch", 1)), StrategyRegistry::AddResult::added);

    uint32_t index = 99;
    ASSERT_TRUE(registry->find(first.id, index));
    EXPECT_EQ(index, 0U);
    const Strategy &found = registry->at(index);
    EXPECT_EQ(found.version, 3U);
    EXPECT_EQ(found.ctx, &ctx);
    EXPECT_EQ(found.decide, &decide_nothing);
    EXPECT_EQ(found.max_intents_per_call, 4U);
    ASSERT_TRUE(registry->find(strategy("watch", 1).id, index));
    EXPECT_EQ(index, 1U);
}

// C-POLICY-010: a strategy is replaced only across a restart, so a second
// version of an id is a duplicate like the same version, even when full.
TEST(StrategyRegistry, refuses_another_version_of_an_id_and_registrations_beyond_capacity) {
    Owned<StrategyRegistry> registry(1);
    ASSERT_EQ(registry->add(strategy("plan", 1)), StrategyRegistry::AddResult::added);
    EXPECT_EQ(registry->add(strategy("plan", 1)), StrategyRegistry::AddResult::duplicate);
    EXPECT_EQ(registry->add(strategy("plan", 2)), StrategyRegistry::AddResult::duplicate);
    EXPECT_EQ(registry->add(strategy("watch", 1)), StrategyRegistry::AddResult::full);
    uint32_t index = 0;
    EXPECT_FALSE(registry->find(strategy("watch", 1).id, index));
    ASSERT_TRUE(registry->find(strategy("plan", 1).id, index));
    EXPECT_EQ(registry->at(index).version, 1U);
}

TEST(StrategyRegistry, ids_match_byte_for_byte) {
    Owned<StrategyRegistry> registry(1);
    ASSERT_EQ(registry->add(strategy("plan", 1)), StrategyRegistry::AddResult::added);
    uint32_t index = 0;
    for (const char *other : {"pla", "plans", "plan2"}) {
        EXPECT_FALSE(registry->find(strategy(other, 1).id, index)) << other;
    }
}

// C-LIFE-003: each goal type's strategy is resolved by id, whatever the order
// of registration; a type with no strategy id gets none.
TEST(GoalTypeRegistry, link_strategies_resolves_each_strategy_id) {
    Owned<StrategyRegistry> strategies(2);
    Owned<GoalTypeRegistry> types(3);
    ASSERT_EQ(types->add(goal_type("reminder", "watch")), GoalTypeRegistry::AddResult::added);
    ASSERT_EQ(types->add(goal_type("folder", "")), GoalTypeRegistry::AddResult::added);
    ASSERT_EQ(types->add(goal_type("timer", "plan")), GoalTypeRegistry::AddResult::added);
    ASSERT_EQ(strategies->add(strategy("plan", 1)), StrategyRegistry::AddResult::added);
    ASSERT_EQ(strategies->add(strategy("watch", 1)), StrategyRegistry::AddResult::added);

    ASSERT_TRUE(types->link_strategies(*strategies));
    EXPECT_EQ(types->at(0).strategy_index, 1U);
    EXPECT_EQ(types->at(1).strategy_index, GoalType::no_strategy);
    EXPECT_EQ(types->at(2).strategy_index, 0U);
}

// ayni_start stays in CONFIGURING on a missing strategy, so registering it
// and linking again succeeds.
TEST(GoalTypeRegistry, link_strategies_fails_on_a_missing_strategy_and_may_run_again) {
    Owned<StrategyRegistry> strategies(2);
    Owned<GoalTypeRegistry> types(2);
    ASSERT_EQ(types->add(goal_type("reminder", "plan")), GoalTypeRegistry::AddResult::added);
    ASSERT_EQ(types->add(goal_type("timer", "watch")), GoalTypeRegistry::AddResult::added);
    ASSERT_EQ(strategies->add(strategy("plan", 1)), StrategyRegistry::AddResult::added);
    EXPECT_FALSE(types->link_strategies(*strategies));

    ASSERT_EQ(strategies->add(strategy("watch", 1)), StrategyRegistry::AddResult::added);
    ASSERT_TRUE(types->link_strategies(*strategies));
    EXPECT_EQ(types->at(0).strategy_index, 0U);
    EXPECT_EQ(types->at(1).strategy_index, 1U);
}

} // namespace
