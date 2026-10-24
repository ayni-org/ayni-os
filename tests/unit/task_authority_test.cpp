#include "work/task/task_authority_records.h"

#include "foundation/kernel_id.h"
#include "foundation/little_endian.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

namespace {
using namespace ayni::work;
constexpr uint64_t task_id = UINT64_C(0x100000020);
constexpr uint64_t goal_id = UINT64_C(0x100000010);
constexpr uint64_t parent_id = UINT64_C(0x100000008);
constexpr uint64_t grant_id = UINT64_C(0x100000030);
constexpr uint32_t slots = 2;
constexpr uint32_t requirements = 2;
constexpr uint32_t goal_at = 8;
constexpr uint32_t binding_at = 16;
constexpr uint32_t transition_at = 24;
constexpr uint32_t commit_at = 32;

TaskAuthorityRecord admitted(uint32_t count = requirements) {
    TaskAuthorityRecord record{};
    record.task_id = task_id;
    record.ceiling = {goal_id, 5};
    record.stamp = {7, 9};
    record.count = count;
    for (uint32_t index = 0; index < count; ++index) {
        record.references[index] = {grant_id + index, 1};
    }
    return record;
}

struct Table {
    std::vector<uint64_t> arena;
    TaskAuthorityTable table;
    explicit Table(uint32_t count = requirements)
        : arena((TaskAuthorityTable::storage_size({slots, count}) + sizeof(uint64_t) - 1U) /
                sizeof(uint64_t)),
          table(arena.data(), {slots, count}) {}
};

std::vector<uint8_t> encode(const TaskAuthorityRecord &record) {
    std::vector<uint8_t> bytes(task_authority_record_max(record.count));
    write_task_authority_record(record, bytes.data());
    return bytes;
}

// ADR-0038 decision 2: a proposal may miss authority, a Task admission may not.
TEST(WorkAuthorityFacts, missing_and_selected_references_share_one_shape_rule) {
    EXPECT_TRUE(valid_authority_reference({}));
    EXPECT_FALSE(valid_selected_authority_reference({}));
    EXPECT_FALSE(valid_authority_reference({0, 1}));
    EXPECT_FALSE(valid_authority_reference({grant_id, 0}));
    EXPECT_FALSE(valid_authority_reference({1, 1}));
    EXPECT_TRUE(valid_selected_authority_reference({grant_id, 1}));
    EXPECT_TRUE(valid_binding_reference({}));
    EXPECT_FALSE(valid_binding_reference({0, 1}));
    EXPECT_FALSE(valid_binding_reference({goal_id, 0}));
    EXPECT_FALSE(valid_binding_reference({1, 1}));
}

// ADR-0038 decision 3: the parent binding is a historical admission, not a mirror.
TEST(WorkAuthorityFacts, goal_binding_names_its_exact_parent_and_monotone_admission) {
    const ayni::foundation::TransitionStamp stamp{11, 12};
    GoalBinding binding{10, {}, {}};
    EXPECT_TRUE(valid_goal_binding(binding, goal_id, 0, stamp));
    EXPECT_FALSE(valid_goal_binding(binding, 1, 0, stamp));
    EXPECT_FALSE(valid_goal_binding(binding, goal_id, parent_id, stamp));
    EXPECT_FALSE(valid_goal_binding(binding, goal_id, 0, {0, stamp.commit_seq}));
    binding.parent = {parent_id, 9};
    EXPECT_TRUE(valid_goal_binding(binding, goal_id, parent_id, stamp));
    EXPECT_FALSE(valid_goal_binding(binding, goal_id, goal_id, stamp));
    binding.parent = {goal_id, 9};
    EXPECT_FALSE(valid_goal_binding(binding, goal_id, goal_id, stamp));
    binding.parent = {parent_id, binding.established_seq};
    EXPECT_FALSE(valid_goal_binding(binding, goal_id, parent_id, stamp));
    binding.parent.commit_seq = 0;
    EXPECT_FALSE(valid_goal_binding(binding, goal_id, parent_id, stamp));
    binding = {};
    EXPECT_FALSE(valid_goal_binding(binding, goal_id, 0, stamp));
    binding.established_seq = stamp.commit_seq + 1U;
    EXPECT_FALSE(valid_goal_binding(binding, goal_id, 0, stamp));
    binding.established_seq = stamp.commit_seq;
    EXPECT_TRUE(valid_goal_binding(binding, goal_id, 0, stamp));
    binding.input = {ayni::foundation::max_boot_epoch + 1U, 0};
    EXPECT_FALSE(valid_goal_binding(binding, goal_id, 0, stamp));
}

// ADR-0038 decision 3: ingress incarnation is a boot epoch, not an allocated ID.
TEST(WorkAuthorityFacts, input_frontier_allows_empty_sequences_but_not_unknown_incarnations) {
    EXPECT_TRUE(valid_input_resume_boundary({}));
    EXPECT_FALSE(valid_input_resume_boundary({0, 1}));
    EXPECT_TRUE(valid_input_resume_boundary({1, 0}));
    EXPECT_TRUE(valid_input_resume_boundary({ayni::foundation::max_boot_epoch, UINT64_MAX}));
    EXPECT_FALSE(valid_input_resume_boundary({ayni::foundation::max_boot_epoch + 1U, 1}));
}

// ADR-0038 decision 2: no Task proposal, implicit zero needs or partial reference.
TEST(TaskAuthority, record_shape_rejects_invalid_identity_stamp_phase_and_last_reference) {
    const auto good = admitted(AYNI_GOAL_REQUIREMENT_CAPACITY_MAX);
    ASSERT_TRUE(valid_task_authority_record(good));
    for (const auto id : {UINT64_C(0), UINT64_C(1), UINT64_C(0x100000000)}) {
        auto record = good;
        record.task_id = id;
        EXPECT_FALSE(valid_task_authority_record(record));
        record = good;
        record.ceiling.goal_id = id;
        EXPECT_FALSE(valid_task_authority_record(record));
    }
    auto record = good;
    record.ceiling.goal_id = record.task_id;
    EXPECT_FALSE(valid_task_authority_record(record));
    record = good;
    record.ceiling.commit_seq = 0;
    EXPECT_FALSE(valid_task_authority_record(record));
    record = good;
    record.ceiling.commit_seq = record.stamp.commit_seq + 1U;
    EXPECT_FALSE(valid_task_authority_record(record));
    record = good;
    record.stamp = {};
    EXPECT_FALSE(valid_task_authority_record(record));
    record = good;
    record.stamp.transition_id = 0;
    EXPECT_FALSE(valid_task_authority_record(record));
    record = good;
    record.stamp.commit_seq = 0;
    EXPECT_FALSE(valid_task_authority_record(record));
    for (const auto phase : {0U, 3U}) {
        record = good;
        record.phase = static_cast<TaskAuthorityPhase>(phase);
        EXPECT_FALSE(valid_task_authority_record(record));
    }
    record = good;
    record.count += 1U;
    EXPECT_FALSE(valid_task_authority_record(record));
    record = good;
    record.references.back() = {};
    EXPECT_FALSE(valid_task_authority_record(record));
    record = admitted(0);
    EXPECT_TRUE(valid_task_authority_record(record));
    record.phase = TaskAuthorityPhase::paused;
    EXPECT_TRUE(valid_task_authority_record(record));
}

// ADR-0038 decision 4: stable slots and exact identities prevent transfer on reuse.
TEST(TaskAuthority, explicit_empty_admission_and_slot_reuse_preserve_identity) {
    Table table;
    TaskAuthorityRecord out{};
    out.task_id = task_id + 1U;
    EXPECT_FALSE(table.table.contains(0, 0));
    EXPECT_FALSE(table.table.read(1, task_id, out));
    EXPECT_EQ(out.task_id, task_id + 1U);
    auto record = admitted(0);
    ASSERT_EQ(table.table.install_validated(1, record), AYNI_OK);
    EXPECT_FALSE(table.table.contains(0, task_id));
    ASSERT_TRUE(table.table.read(1, task_id, out));
    EXPECT_EQ(out.count, 0U);
    EXPECT_EQ(out.ceiling.commit_seq, record.ceiling.commit_seq);
    table.table.clear(slots);
    EXPECT_TRUE(table.table.contains(1, task_id));
    table.table.clear(1);
    record.task_id += 1U;
    ASSERT_EQ(table.table.install_validated(1, record), AYNI_OK);
    EXPECT_FALSE(table.table.read(1, task_id, out));
    ASSERT_TRUE(table.table.read(1, record.task_id, out));
    table.table.clear_all();
    EXPECT_FALSE(table.table.contains(1, record.task_id));
}

// ADR-0038 decisions 2/4: Task owns copied selections; refusals preserve every slot.
TEST(TaskAuthority, copied_references_and_capacity_failures_preserve_the_arena) {
    Table table;
    auto record = admitted();
    ASSERT_EQ(table.table.install_validated(1, record), AYNI_OK);
    record.references[0].grant_id += 1U;
    TaskAuthorityRecord out{};
    ASSERT_TRUE(table.table.read(1, task_id, out));
    EXPECT_EQ(out.references[0].grant_id, grant_id);
    EXPECT_EQ(out.references[1].grant_id, grant_id + 1U);
    const auto before = table.arena;
    record.count += 1U;
    EXPECT_EQ(table.table.install_validated(0, record), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(table.table.restore_validated(1, record), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(table.arena, before);
    record = admitted();
    EXPECT_EQ(table.table.install_validated(slots, record), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(table.table.restore_validated(slots, record), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(table.arena, before);
    EXPECT_FALSE(table.table.read(slots, task_id, out));
}

// ADR-0038 decision 4: restore permits only a monotone history of the same Task ceiling.
TEST(TaskAuthority, same_binding_can_pause_but_cannot_resume_or_change_references) {
    Table table;
    const auto original = admitted();
    ASSERT_EQ(table.table.restore_validated(0, original), AYNI_OK);
    auto paused = original;
    paused.stamp = {8, 10};
    paused.phase = TaskAuthorityPhase::paused;
    ASSERT_EQ(table.table.restore_validated(0, paused), AYNI_OK);
    const auto before = table.arena;
    std::vector<TaskAuthorityRecord> invalid;
    auto next = paused;
    next.stamp = {9, 11};
    next.phase = TaskAuthorityPhase::authorized;
    invalid.push_back(next);
    next.phase = TaskAuthorityPhase::paused;
    next.references[0].grant_id += 1U;
    invalid.push_back(next);
    next = paused;
    next.stamp = {9, 11};
    next.references[1].revision += 1U;
    invalid.push_back(next);
    for (const auto &bad : invalid) {
        EXPECT_EQ(table.table.restore_validated(0, bad), AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(table.arena, before);
    }
    next = invalid.front();
    next.ceiling.commit_seq += 1U;
    next.references[1].grant_id += 1U;
    ASSERT_EQ(table.table.restore_validated(0, next), AYNI_OK);
    TaskAuthorityRecord out{};
    ASSERT_TRUE(table.table.read(0, task_id, out));
    EXPECT_EQ(out.phase, TaskAuthorityPhase::authorized);
    EXPECT_EQ(out.references[1].grant_id, next.references[1].grant_id);
}

// ADR-0038 decision 4: replay cannot silently replace a Task or widen its ceiling.
TEST(TaskAuthority, restore_rejects_identity_count_stamp_and_binding_regressions) {
    Table table;
    const auto original = admitted();
    ASSERT_EQ(table.table.install_validated(0, original), AYNI_OK);
    const auto before = table.arena;
    for (uint32_t scenario = 0; scenario < 6U; ++scenario) {
        auto bad = original;
        bad.stamp = {8, 10};
        switch (scenario) {
        case 0:
            ++bad.task_id;
            break;
        case 1:
            ++bad.ceiling.goal_id;
            break;
        case 2:
            --bad.count;
            break;
        case 3:
            bad.stamp.commit_seq = original.stamp.commit_seq;
            break;
        case 4:
            bad.stamp.transition_id = original.stamp.transition_id;
            break;
        default:
            --bad.ceiling.commit_seq;
            break;
        }
        ASSERT_TRUE(valid_task_authority_record(bad));
        EXPECT_EQ(table.table.restore_validated(0, bad), AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(table.arena, before);
    }
}

// ADR-0038 decision 5, records.md type18/v1: fixed little-endian layout at both limits.
TEST(TaskAuthorityCodec, empty_and_maximum_admissions_round_trip_without_scope_copies) {
    for (const auto count : {0U, AYNI_GOAL_REQUIREMENT_CAPACITY_MAX}) {
        auto record = admitted(count);
        record.phase = TaskAuthorityPhase::paused;
        const auto bytes = encode(record);
        TaskAuthorityRecord out{};
        ASSERT_TRUE(
            read_task_authority_record(bytes.data(), static_cast<uint32_t>(bytes.size()), out));
        EXPECT_EQ(bytes.size(), task_authority_record_max(count));
        EXPECT_EQ(out.task_id, record.task_id);
        EXPECT_EQ(out.ceiling.goal_id, record.ceiling.goal_id);
        EXPECT_EQ(out.ceiling.commit_seq, record.ceiling.commit_seq);
        EXPECT_EQ(out.stamp.transition_id, record.stamp.transition_id);
        EXPECT_EQ(out.stamp.commit_seq, record.stamp.commit_seq);
        EXPECT_EQ(out.phase, record.phase);
        ASSERT_EQ(out.count, count);
        for (uint32_t index = 0; index < count; ++index) {
            EXPECT_EQ(out.references[index].grant_id, record.references[index].grant_id);
            EXPECT_EQ(out.references[index].revision, record.references[index].revision);
        }
        EXPECT_EQ(encode(out), bytes);
    }
}

// ADR-0038 decision 5: reader failures never publish an otherwise valid prefix.
TEST(TaskAuthorityCodec, every_truncation_trailing_bytes_and_malformed_fact_preserve_output) {
    const auto bytes = encode(admitted());
    TaskAuthorityRecord out = admitted(0);
    std::array<uint8_t, sizeof(out)> before{};
    std::memcpy(before.data(), &out, sizeof(out));
    const auto refused = [&](const uint8_t *body, std::size_t size) {
        EXPECT_FALSE(read_task_authority_record(body, static_cast<uint32_t>(size), out));
        EXPECT_EQ(std::memcmp(before.data(), &out, sizeof(out)), 0);
    };
    refused(nullptr, bytes.size());
    for (uint32_t size = 0; size < bytes.size(); ++size) {
        refused(bytes.data(), size);
    }
    auto bad = bytes;
    bad.push_back(0);
    refused(bad.data(), bad.size());
    for (const auto offset :
         {0U, goal_at, binding_at, transition_at, commit_at, task_authority_record_fixed_size}) {
        bad = bytes;
        ayni::foundation::store_le64(bad.data() + offset, 0);
        refused(bad.data(), bad.size());
    }
    bad = bytes;
    ayni::foundation::store_le32(bad.data() + task_authority_phase_at, 0);
    refused(bad.data(), bad.size());
    bad = bytes;
    ayni::foundation::store_le32(bad.data() + task_authority_count_at,
                                 AYNI_GOAL_REQUIREMENT_CAPACITY_MAX + 1U);
    refused(bad.data(), bad.size());
}
} // namespace
