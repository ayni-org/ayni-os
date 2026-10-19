// Catalog ids and goal type versions owned by catalog (C-POLICY-009), and
// the trigger kinds a goal type accepts (ADR-0018).

#include "catalog/catalog_id.h"
#include "catalog/goal_type/goal_type_registry.h"

#include <ayni/ayni.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using ayni::catalog::CatalogId;
using ayni::catalog::GoalType;
using ayni::catalog::GoalTypeRegistry;
using Kind = GoalTypeRegistry::Lookup::Kind;

ayni_string_v1 text(const char *value) {
    return {value, static_cast<uint32_t>(std::strlen(value))};
}

GoalType goal_type(const char *id, uint32_t version, ayni_goal_validate_fn validate = nullptr,
                   void *ctx = nullptr) {
    GoalType type{};
    EXPECT_TRUE(type.id.assign(text(id)));
    type.version = version;
    type.ctx = ctx;
    type.validate = validate;
    return type;
}

class Registry {
  public:
    explicit Registry(uint32_t capacity)
        : storage_(GoalTypeRegistry::storage_size(capacity) / sizeof(uint64_t) + 1U),
          registry_(storage_.data(), capacity) {}
    GoalTypeRegistry &operator*() { return registry_; }
    GoalTypeRegistry *operator->() { return &registry_; }

  private:
    std::vector<uint64_t> storage_;
    GoalTypeRegistry registry_;
};

TEST(CatalogId, accepts_lowercase_digits_and_three_marks) {
    EXPECT_TRUE(CatalogId::is_valid(text("reminder")));
    EXPECT_TRUE(CatalogId::is_valid(text("notify.present_v2-b")));
    EXPECT_TRUE(CatalogId::is_valid(text("0")));
    EXPECT_TRUE(CatalogId::is_valid(text("abcdefghijklmnopqrstuvwxyz012345")));
}

TEST(CatalogId, refuses_empty_long_and_other_bytes) {
    EXPECT_FALSE(CatalogId::is_valid(text("")));
    EXPECT_FALSE(CatalogId::is_valid({nullptr, 0}));
    EXPECT_FALSE(CatalogId::is_valid({nullptr, 3}));
    EXPECT_FALSE(CatalogId::is_valid(text("abcdefghijklmnopqrstuvwxyz0123456")));
    for (const char *bad : {"Reminder", "re minder", "re/minder", "re:minder", "\xe6\x8f\x90"}) {
        EXPECT_FALSE(CatalogId::is_valid(text(bad))) << bad;
    }
    const char with_nul[] = {'a', '\0', 'b'};
    EXPECT_FALSE(CatalogId::is_valid({with_nul, 3}));
}

TEST(CatalogId, assign_keeps_the_old_value_on_refusal) {
    CatalogId id;
    ASSERT_TRUE(id.assign(text("reminder")));
    EXPECT_FALSE(id.assign(text("Reminder")));
    EXPECT_TRUE(id.equals(text("reminder")));
    EXPECT_FALSE(id.equals(text("reminde")));
    EXPECT_FALSE(id.equals(text("reminders")));
    EXPECT_FALSE(id.equals({nullptr, 8}));
}

// Several versions of one id coexist; each is found on its own.
TEST(GoalTypeRegistry, finds_each_registered_version) {
    Registry registry(3);
    EXPECT_EQ(registry->add(goal_type("reminder", 1)), GoalTypeRegistry::AddResult::added);
    EXPECT_EQ(registry->add(goal_type("reminder", 2)), GoalTypeRegistry::AddResult::added);
    EXPECT_EQ(registry->add(goal_type("watch", 1)), GoalTypeRegistry::AddResult::added);

    const GoalTypeRegistry::Lookup second = registry->find(text("reminder"), 2);
    ASSERT_EQ(second.kind, Kind::found);
    EXPECT_EQ(registry->at(second.index).version, 2U);
    EXPECT_TRUE(registry->at(second.index).id.equals(text("reminder")));
    EXPECT_EQ(registry->find(text("watch"), 1).kind, Kind::found);
}

TEST(GoalTypeRegistry, tells_an_unknown_id_from_an_unknown_version) {
    Registry registry(2);
    ASSERT_EQ(registry->add(goal_type("reminder", 1)), GoalTypeRegistry::AddResult::added);
    EXPECT_EQ(registry->find(text("reminder"), 2).kind, Kind::unknown_version);
    EXPECT_EQ(registry->find(text("timer"), 1).kind, Kind::unknown_id);
    EXPECT_EQ(registry->find(text("remind"), 1).kind, Kind::unknown_id);
}

TEST(GoalTypeRegistry, refuses_duplicates_and_registrations_beyond_capacity) {
    Registry registry(2);
    ASSERT_EQ(registry->add(goal_type("reminder", 1)), GoalTypeRegistry::AddResult::added);
    EXPECT_EQ(registry->add(goal_type("reminder", 1)), GoalTypeRegistry::AddResult::duplicate);
    ASSERT_EQ(registry->add(goal_type("reminder", 2)), GoalTypeRegistry::AddResult::added);
    EXPECT_EQ(registry->add(goal_type("reminder", 3)), GoalTypeRegistry::AddResult::full);
    // A duplicate is reported as such even when the registry is full.
    EXPECT_EQ(registry->add(goal_type("reminder", 2)), GoalTypeRegistry::AddResult::duplicate);
    EXPECT_EQ(registry->find(text("reminder"), 3).kind, Kind::unknown_version);
}

struct Validator {
    static ayni_status_t check(void *ctx, ayni_bytes_v1 parameters) {
        auto &self = *static_cast<Validator *>(ctx);
        ++self.calls;
        self.seen.assign(parameters.data, parameters.data + parameters.size);
        return parameters.size == 2U ? AYNI_OK : AYNI_INVALID_ARGUMENT;
    }
    int calls = 0;
    std::vector<uint8_t> seen;
};

TEST(GoalTypeRegistry, validator_decides_the_parameters) {
    Registry registry(2);
    Validator validator;
    ASSERT_EQ(registry->add(goal_type("reminder", 1, &Validator::check, &validator)),
              GoalTypeRegistry::AddResult::added);
    const uint8_t two[] = {4, 5};
    const uint8_t three[] = {4, 5, 6};
    const uint32_t index = registry->find(text("reminder"), 1).index;
    EXPECT_TRUE(registry->accepts(index, {two, 2}));
    EXPECT_EQ(validator.seen, (std::vector<uint8_t>{4, 5}));
    EXPECT_FALSE(registry->accepts(index, {three, 3}));
    EXPECT_EQ(validator.calls, 2);
}

TEST(GoalTypeRegistry, type_without_validator_takes_no_parameters) {
    Registry registry(1);
    ASSERT_EQ(registry->add(goal_type("timer", 1)), GoalTypeRegistry::AddResult::added);
    const uint8_t one[] = {1};
    EXPECT_TRUE(registry->accepts(0, {nullptr, 0}));
    EXPECT_FALSE(registry->accepts(0, {one, 1}));
}

TEST(GoalTypeRegistry, trigger_kinds_decide_the_accepted_triggers) {
    Registry registry(1);
    GoalType folder = goal_type("folder", 1);
    folder.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_NONE);
    ASSERT_EQ(registry->add(folder), GoalTypeRegistry::AddResult::added);
    EXPECT_TRUE(registry->accepts_trigger(0, AYNI_TRIGGER_NONE));
    EXPECT_FALSE(registry->accepts_trigger(0, AYNI_TRIGGER_ONCE));
    // Kinds past the mask's width are never accepted, rather than shifted.
    EXPECT_FALSE(registry->accepts_trigger(0, 32U));
    EXPECT_FALSE(registry->accepts_trigger(0, 0xFFFFFFFFU));
}

TEST(GoalTypeRegistry, defines_none_and_once_only) {
    EXPECT_TRUE(GoalTypeRegistry::defines_trigger(AYNI_TRIGGER_NONE));
    EXPECT_TRUE(GoalTypeRegistry::defines_trigger(AYNI_TRIGGER_ONCE));
    for (const ayni_trigger_kind_t kind : {2U, 31U, 32U, 0xFFFFFFFFU}) {
        EXPECT_FALSE(GoalTypeRegistry::defines_trigger(kind)) << kind;
    }
}

} // namespace
