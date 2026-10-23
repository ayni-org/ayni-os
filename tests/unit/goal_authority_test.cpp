#include "work/goal/goal_authority_records.h"

#include "authority/scope/scope.h"
#include "catalog/capability/field_domain.h"
#include "foundation/kernel_id.h"
#include "foundation/little_endian.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

namespace {
using namespace ayni::work;
constexpr uint64_t goal_id = UINT64_C(0x100000010);
constexpr uint64_t another_goal_id = UINT64_C(0x100000011);
constexpr uint64_t grant_id = UINT64_C(0x100000020);
constexpr uint32_t bytes_capacity = 256;
constexpr uint32_t slots = 2;

// ADR-0037 decisions 2/3: fixture owns both canonical regions for the borrowed
// record. There is deliberately no current catalog when decoding saved facts.
struct Input {
    std::array<uint8_t, bytes_capacity> domain{};
    std::array<uint8_t, bytes_capacity> scope{};
    GoalAuthorityRecord record{};

    explicit Input(uint32_t fields = 1) {
        std::array<ayni_authority_field_desc_v1, AYNI_AUTHORITY_FIELD_COUNT_MAX> descriptions{};
        for (uint32_t index = 0; index < fields; ++index) {
            ayni_authority_value_set_v1 values{};
            values.struct_size = sizeof(values);
            values.kind = AYNI_AUTHORITY_I64;
            values.unit = {"count", 5};
            values.values.integer = {-10, 10};
            descriptions[index] = {sizeof(descriptions[index]), index + 1U,
                                   AYNI_AUTHORITY_PARAMETER, 0, values};
        }
        uint32_t domain_size = 0;
        EXPECT_EQ(ayni::catalog::encode_domain(descriptions.data(), fields, domain.data(),
                                               domain.size(), domain_size),
                  AYNI_OK);
        const ayni_scope_v1 whole{sizeof(whole), 0, nullptr};
        uint32_t scope_size = 0;
        EXPECT_EQ(ayni::authority::normalize_scope({domain.data(), domain_size}, whole,
                                                   scope.data(), scope.size(), scope_size),
                  AYNI_OK);
        record.goal_id = goal_id;
        record.stamp = {7, 9};
        record.count = 1;
        auto &requirement = record.requirements[0];
        EXPECT_TRUE(requirement.capability_id.assign({"test.cap", 8}));
        requirement.capability_version = 3;
        requirement.source_domain = {domain.data(), domain_size};
        requirement.scope = {scope.data(), scope_size};
        requirement.dependency = {grant_id, 1};
        EXPECT_TRUE(valid_goal_authority_record(record));
    }

    std::vector<uint8_t> encode() const {
        std::vector<uint8_t> bytes(goal_authority_record_size(record));
        write_goal_authority_record(record, bytes.data());
        return bytes;
    }
};

struct Table {
    std::vector<uint64_t> storage;
    GoalAuthorityTable table;
    explicit Table(uint32_t requirements = 2, uint32_t bytes = bytes_capacity)
        : storage((GoalAuthorityTable::storage_size({slots, requirements, bytes}) +
                   sizeof(uint64_t) - 1U) /
                  sizeof(uint64_t)),
          table(storage.data(), {slots, requirements, bytes}) {}
};

// ADR-0037 decision 4: missing metadata cannot silently become zero authority.
TEST(GoalAuthority, an_explicit_empty_ceiling_is_distinct_from_absent_metadata) {
    Table table;
    GoalAuthorityRecord out{};
    out.goal_id = another_goal_id;
    EXPECT_FALSE(table.table.read(0, goal_id, out));
    EXPECT_EQ(out.goal_id, another_goal_id);
    Input input;
    input.record.count = 0;
    ASSERT_TRUE(valid_goal_authority_record(input.record));
    ASSERT_EQ(table.table.install_validated(0, input.record), AYNI_OK);
    ASSERT_TRUE(table.table.read(0, goal_id, out));
    EXPECT_EQ(out.count, 0U);
    EXPECT_EQ(out.phase, GoalAuthorityPhase::authorized);
    EXPECT_EQ(out.stamp.transition_id, 7U);
    table.table.clear(0);
    EXPECT_FALSE(table.table.read(0, goal_id, out));
}

// ADR-0037 decisions 2/4: phase describes completeness of saved selections.
TEST(GoalAuthority, proposals_need_a_missing_dependency_and_other_phases_keep_complete_selections) {
    Input input;
    input.record.phase = GoalAuthorityPhase::proposed;
    EXPECT_FALSE(valid_goal_authority_record(input.record));
    input.record.requirements[0].dependency = {};
    EXPECT_TRUE(valid_goal_authority_record(input.record));
    input.record.phase = GoalAuthorityPhase::authorized;
    EXPECT_FALSE(valid_goal_authority_record(input.record));
    input.record.phase = GoalAuthorityPhase::paused;
    EXPECT_FALSE(valid_goal_authority_record(input.record));
    input.record.requirements[0].dependency = {grant_id, 1};
    EXPECT_TRUE(valid_goal_authority_record(input.record));
    input.record.count = 0;
    EXPECT_TRUE(valid_goal_authority_record(input.record));
    input.record.phase = GoalAuthorityPhase::proposed;
    EXPECT_FALSE(valid_goal_authority_record(input.record));
}

// ADR-0037 decision 3: struct admission checks identities and exact domain facts.
TEST(GoalAuthority, malformed_identity_stamp_capability_or_reference_is_refused) {
    Input input;
    const auto good = input.record;
    const auto rejected = [&](const GoalAuthorityRecord &record) {
        EXPECT_FALSE(valid_goal_authority_record(record));
    };
    for (uint64_t invalid : {UINT64_C(0), UINT64_C(1), UINT64_C(0x100000000)}) {
        auto record = good;
        record.goal_id = invalid;
        rejected(record);
        record = good;
        record.requirements[0].dependency.grant_id = invalid;
        rejected(record);
    }
    auto record = good;
    record.stamp.transition_id = 0;
    rejected(record);
    record = good;
    record.stamp.commit_seq = 0;
    rejected(record);
    record = good;
    record.phase = static_cast<GoalAuthorityPhase>(0);
    rejected(record);
    record = good;
    record.requirements[0].capability_id = {};
    rejected(record);
    record = good;
    record.requirements[0].capability_version = 0;
    rejected(record);
    record = good;
    record.requirements[0].dependency.revision = 0;
    rejected(record);
    record = good;
    record.requirements[0].source_domain = {};
    rejected(record);
    record = good;
    record.requirements[0].scope = {};
    rejected(record);
}

// ADR-0037 decision 2: the maximum is inclusive and every requirement is checked.
TEST(GoalAuthority, the_last_requirement_at_the_absolute_limit_is_validated) {
    Input input;
    input.record.count = goal_requirement_capacity_max;
    for (auto &requirement : input.record.requirements) {
        requirement = input.record.requirements[0];
    }
    ASSERT_TRUE(valid_goal_authority_record(input.record));
    const auto bytes = input.encode();
    GoalAuthorityRecord decoded{};
    ASSERT_TRUE(read_goal_authority_record(bytes.data(), bytes.size(), decoded));
    EXPECT_EQ(decoded.count, goal_requirement_capacity_max);
    input.record.requirements.back().capability_version = 0;
    EXPECT_FALSE(valid_goal_authority_record(input.record));
    input.record.count = goal_requirement_capacity_max + 1U;
    EXPECT_FALSE(valid_goal_authority_record(input.record));
}

// records.md type 17/v1: exact round trip, including a partial proposal's refs.
TEST(GoalAuthorityRecord, original_domain_scope_and_selected_identities_round_trip_exactly) {
    Input input;
    input.record.count = 2;
    input.record.requirements[1] = input.record.requirements[0];
    input.record.requirements[1].dependency = {};
    input.record.phase = GoalAuthorityPhase::proposed;
    ASSERT_TRUE(valid_goal_authority_record(input.record));
    const auto bytes = input.encode();
    GoalAuthorityRecord decoded{};
    ASSERT_TRUE(read_goal_authority_record(bytes.data(), bytes.size(), decoded));
    EXPECT_EQ(decoded.goal_id, goal_id);
    EXPECT_EQ(decoded.phase, GoalAuthorityPhase::proposed);
    EXPECT_EQ(decoded.stamp.transition_id, 7U);
    EXPECT_EQ(decoded.stamp.commit_seq, 9U);
    EXPECT_EQ(decoded.requirements[0].dependency.grant_id, grant_id);
    EXPECT_EQ(decoded.requirements[0].dependency.revision, 1U);
    EXPECT_EQ(decoded.requirements[1].dependency.grant_id, 0U);
    EXPECT_EQ(decoded.requirements[0].capability_version, 3U);
    std::vector<uint8_t> rewritten(bytes.size());
    write_goal_authority_record(decoded, rewritten.data());
    EXPECT_EQ(rewritten, bytes);
}

// records.md type 17/v1: no decoder publishes a valid prefix from a bad body.
TEST(GoalAuthorityRecord, every_truncation_trailing_byte_and_bad_count_preserves_output) {
    Input input;
    auto bytes = input.encode();
    GoalAuthorityRecord out{};
    out.goal_id = another_goal_id;
    for (uint32_t size = 0; size < bytes.size(); ++size) {
        EXPECT_FALSE(read_goal_authority_record(bytes.data(), size, out));
        EXPECT_EQ(out.goal_id, another_goal_id);
    }
    EXPECT_FALSE(read_goal_authority_record(nullptr, bytes.size(), out));
    bytes.push_back(0);
    EXPECT_FALSE(read_goal_authority_record(bytes.data(), bytes.size(), out));
    bytes.pop_back();
    // White-box: final u32 of the fixed header is the declared count.
    ayni::foundation::store_le32(bytes.data() + goal_authority_record_fixed_size - sizeof(uint32_t),
                                 goal_requirement_capacity_max + 1U);
    EXPECT_FALSE(read_goal_authority_record(bytes.data(), bytes.size(), out));
    EXPECT_EQ(out.goal_id, another_goal_id);
}

// ADR-0037 decisions 2/3: copied views survive caller mutation and slot reuse
// cannot hand the old Goal another Goal's authority.
TEST(GoalAuthority, installed_regions_are_owned_and_slot_reuse_requires_the_exact_goal) {
    Table table;
    Input input;
    const auto expected = input.encode();
    ASSERT_EQ(table.table.install_validated(0, input.record), AYNI_OK);
    input.domain.fill(0);
    input.scope.fill(0);
    GoalAuthorityRecord saved{};
    ASSERT_TRUE(table.table.read(0, goal_id, saved));
    ASSERT_TRUE(valid_goal_authority_record(saved));
    std::vector<uint8_t> actual(goal_authority_record_size(saved));
    write_goal_authority_record(saved, actual.data());
    EXPECT_EQ(actual, expected);
    Input replacement;
    replacement.record.goal_id = another_goal_id;
    ASSERT_EQ(table.table.install_validated(0, replacement.record), AYNI_OK);
    EXPECT_FALSE(table.table.read(0, goal_id, saved));
    EXPECT_TRUE(table.table.read(0, another_goal_id, saved));
    EXPECT_FALSE(table.table.read(slots, another_goal_id, saved));
}

// ADR-0037 decision 3: no partial mutation when capacity fails after a prefix.
TEST(GoalAuthority, configured_requirement_and_byte_limits_refuse_without_overwriting_any_slot) {
    Table table(1, 64);
    Input input;
    ASSERT_EQ(table.table.install_validated(0, input.record), AYNI_OK);
    ASSERT_EQ(table.table.install_validated(1, input.record), AYNI_OK);
    auto too_many = input.record;
    too_many.count = 2;
    too_many.requirements[1] = too_many.requirements[0];
    ASSERT_TRUE(valid_goal_authority_record(too_many));
    EXPECT_EQ(table.table.install_validated(0, too_many), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(table.table.install_validated(slots, input.record), AYNI_CAPACITY_EXCEEDED);
    Input too_large(4);
    ASSERT_GT(too_large.record.requirements[0].source_domain.size, 64U);
    EXPECT_EQ(table.table.install_validated(0, too_large.record), AYNI_CAPACITY_EXCEEDED);
    for (uint32_t index = 0; index < slots; ++index) {
        GoalAuthorityRecord saved{};
        ASSERT_TRUE(table.table.read(index, goal_id, saved));
        EXPECT_EQ(saved.count, 1U);
        EXPECT_EQ(saved.requirements[0].source_domain.size,
                  input.record.requirements[0].source_domain.size);
    }
    table.table.clear(slots);
    GoalAuthorityRecord saved{};
    EXPECT_TRUE(table.table.read(0, goal_id, saved));
    table.table.clear_all();
    EXPECT_FALSE(table.table.read(0, goal_id, saved));
    EXPECT_FALSE(table.table.read(1, goal_id, saved));
}

} // namespace
