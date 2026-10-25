// ADR-0038 decision 12: historical command evidence owns its original binding
// and selected references; it never borrows the Goal's later authorization.
#include "foundation/kernel_id.h"
#include "foundation/little_endian.h"
#include "kernel/command/command_records.h"
#include "kernel/command/command_table.h"
#include "kernel/command/work_authority_result.h"
#include "kernel/persistence/work_replay.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace {
using namespace ayni::kernel;

uint64_t id(uint32_t sequence) {
    ayni::foundation::IdAllocator ids;
    ids.start(1, sequence);
    uint64_t value = 0;
    EXPECT_TRUE(ids.allocate(value));
    return value;
}

WorkAuthorityResult result(uint32_t count) {
    WorkAuthorityResult fact{};
    fact.transition_id = 31;
    fact.binding = {42, {id(2), 30}, {1, 81}};
    fact.count = count;
    for (uint32_t at = 0; at < count; ++at) {
        fact.references[at] = {id(100U + at), 1};
    }
    return fact;
}

void expect_same(const WorkAuthorityResult &left, const WorkAuthorityResult &right) {
    EXPECT_EQ(left.transition_id, right.transition_id);
    EXPECT_EQ(left.binding.established_seq, right.binding.established_seq);
    EXPECT_EQ(left.binding.parent.goal_id, right.binding.parent.goal_id);
    EXPECT_EQ(left.binding.parent.commit_seq, right.binding.parent.commit_seq);
    EXPECT_EQ(left.binding.input.incarnation, right.binding.input.incarnation);
    EXPECT_EQ(left.binding.input.through, right.binding.input.through);
    EXPECT_EQ(left.count, right.count);
    for (uint32_t at = 0; at < left.references.size(); ++at) {
        EXPECT_EQ(left.references[at].grant_id, right.references[at].grant_id);
        EXPECT_EQ(left.references[at].revision, right.references[at].revision);
    }
}

TEST(WorkAuthorityResult, zero_and_maximum_results_own_their_original_binding_and_references) {
    for (const auto count : {0U, AYNI_GOAL_REQUIREMENT_CAPACITY_MAX}) {
        const auto original = result(count);
        ASSERT_TRUE(valid_work_authority_result(original, id(1)));
        std::vector<uint8_t> bytes(work_authority_result_size(count));
        write_work_authority_result(original, bytes.data());
        WorkAuthorityResult decoded{};
        ASSERT_TRUE(read_work_authority_result({bytes.data(), static_cast<uint32_t>(bytes.size())},
                                               id(1), decoded));
        std::fill(bytes.begin(), bytes.end(), 0);
        expect_same(decoded, original);
    }
}

TEST(WorkAuthorityResult, every_truncation_and_trailing_byte_preserves_all_output) {
    for (const auto count : {0U, 1U, AYNI_GOAL_REQUIREMENT_CAPACITY_MAX}) {
        const auto original = result(count);
        const auto size = work_authority_result_size(original.count);
        std::vector<uint8_t> bytes(size + 1U);
        write_work_authority_result(original, bytes.data());
        const auto sentinel = result(3);
        for (uint32_t length = 0; length <= bytes.size(); ++length) {
            if (length == size) {
                continue;
            }
            auto out = sentinel;
            EXPECT_FALSE(read_work_authority_result({bytes.data(), length}, id(1), out));
            expect_same(out, sentinel);
        }
        auto out = sentinel;
        EXPECT_FALSE(read_work_authority_result({nullptr, size}, id(1), out));
        expect_same(out, sentinel);
    }
}

TEST(WorkAuthorityResult, illegal_binding_or_incomplete_selected_reference_is_never_evidence) {
    const auto original = result(1);
    for (unsigned fault = 0; fault < 8U; ++fault) {
        auto fact = original;
        switch (fault) {
        case 0:
            fact.transition_id = 0;
            break;
        case 1:
            fact.binding.established_seq = 0;
            break;
        case 2:
            fact.binding.parent.goal_id = id(1);
            break;
        case 3:
            fact.binding.parent.commit_seq = fact.binding.established_seq;
            break;
        case 4:
            fact.binding.input.incarnation = 0;
            break;
        case 5:
            fact.references[0] = {};
            break;
        case 6:
            fact.references[0].revision = 0;
            break;
        default:
            fact.references[0].grant_id = 1;
        }
        EXPECT_FALSE(valid_work_authority_result(fact, id(1)));
        std::vector<uint8_t> bytes(work_authority_result_size(fact.count));
        // White-box malformed-record production; the normal writer requires
        // validated facts, and the reader must independently refuse these bytes.
        write_work_authority_result(fact, bytes.data());
        auto out = original;
        EXPECT_FALSE(read_work_authority_result({bytes.data(), static_cast<uint32_t>(bytes.size())},
                                                id(1), out));
        expect_same(out, original);
    }
    auto oversized = original;
    oversized.count = AYNI_GOAL_REQUIREMENT_CAPACITY_MAX + 1U;
    EXPECT_FALSE(valid_work_authority_result(oversized, id(1)));
    std::vector<uint8_t> bytes(work_authority_result_size(original.count));
    write_work_authority_result(original, bytes.data());
    // records.md: count follows transition plus five u64 binding words.
    ayni::foundation::store_le32(bytes.data() + work_authority_result_fixed_size - sizeof(uint32_t),
                                 oversized.count);
    auto out = original;
    EXPECT_FALSE(read_work_authority_result({bytes.data(), static_cast<uint32_t>(bytes.size())},
                                            id(1), out));
    expect_same(out, original);
}

ApprovalCommand approval(uint64_t goal_id) {
    ApprovalCommand command{};
    command.expected_revision = 1;
    command.approval.goal_id = goal_id;
    command.approval.missing_mask = 1;
    command.approval.lifetime = AYNI_GRANT_PERMANENT;
    auto &create = command.approval.original;
    create.owner_instance_id = id(10);
    create.parent_goal_id = id(2);
    create.goal_type_id = {"original", 8};
    create.goal_type_version = 1;
    create.trigger.kind = AYNI_TRIGGER_NONE;
    return command;
}

CommandMapping mapping(uint64_t command, uint64_t goal, uint64_t seq) {
    return {77, command, 0, seq, AYNI_INTENT_GOAL_APPROVE, {0, goal, 2, AYNI_OBJECT_GOAL}};
}

// ADR-0038 decision 12: exact request identity and its historical result are
// owned together, including when eviction moves a retained mapping's slot.
TEST(WorkAuthorityResult, command_window_owns_results_across_retry_eviction_and_slot_movement) {
    constexpr uint32_t stride = 512;
    std::vector<uint64_t> arena(CommandTable::storage_size({1, 2, stride}) / sizeof(uint64_t) + 1U);
    CommandTable commands(arena.data(), {1, 2, stride});
    for (uint64_t command_id = 1; command_id <= 3U; ++command_id) {
        const auto goal = id(static_cast<uint32_t>(command_id + 20U));
        const auto input = approval(goal);
        auto fact = result(1);
        fact.binding.input = {};
        fact.transition_id += command_id;
        fact.binding.established_seq += command_id;
        fact.references[0].grant_id = id(static_cast<uint32_t>(command_id + 100U));
        std::vector<uint8_t> bytes(work_authority_result_size(fact.count));
        write_work_authority_result(fact, bytes.data());
        const auto found = commands.find_exact(77, command_id, input);
        ASSERT_EQ(found.status, AYNI_OK);
        ASSERT_EQ(found.lookup, CommandTable::Lookup::unmapped);
        ASSERT_TRUE(commands.insert_exact(mapping(command_id, goal, fact.binding.established_seq),
                                          {bytes.data(), static_cast<uint32_t>(bytes.size())}));
        std::fill(bytes.begin(), bytes.end(), 0);
        const auto retry = commands.find_exact(77, command_id, input);
        ASSERT_EQ(retry.lookup, CommandTable::Lookup::retried);
        ASSERT_NE(retry.mapping, nullptr);
        WorkAuthorityResult retained{};
        ASSERT_TRUE(read_work_authority_result(commands.authorization_result(*retry.mapping), goal,
                                               retained));
        expect_same(retained, fact);
    }
    const auto retry = commands.find_exact(77, 2, approval(id(22)));
    ASSERT_EQ(retry.lookup, CommandTable::Lookup::retried);
    ASSERT_NE(retry.mapping, nullptr);
    WorkAuthorityResult moved{};
    ASSERT_TRUE(
        read_work_authority_result(commands.authorization_result(*retry.mapping), id(22), moved));
    EXPECT_EQ(moved.references[0].grant_id, id(102));
    EXPECT_EQ(moved.binding.established_seq, 44U);
    EXPECT_EQ(commands.floor(77), 1U);
}

// C-LIMIT-001 / ADR-0038: capacity includes the historical result, and refusal
// cannot evict an existing command or publish an incomplete authorization.
TEST(WorkAuthorityResult, result_capacity_and_parent_binding_refusals_preserve_the_window) {
    const auto input = approval(id(1));
    uint32_t input_size = 0;
    ASSERT_EQ(measure_approval_command(input, input_size), AYNI_OK);
    auto fact = result(1);
    fact.binding.input = {};
    std::vector<uint8_t> bytes(work_authority_result_size(fact.count));
    write_work_authority_result(fact, bytes.data());
    const auto stride = input_size + static_cast<uint32_t>(bytes.size()) - 1U;
    std::vector<uint64_t> arena(CommandTable::storage_size({1, 1, stride}) / sizeof(uint64_t) + 1U);
    CommandTable commands(arena.data(), {1, 1, stride});
    ASSERT_EQ(commands.find_exact(77, 1, input).lookup, CommandTable::Lookup::unmapped);
    EXPECT_FALSE(commands.insert_exact(mapping(1, id(1), fact.binding.established_seq),
                                       {bytes.data(), static_cast<uint32_t>(bytes.size())}));
    EXPECT_EQ(commands.size(), 0U);
    EXPECT_EQ(commands.floor(77), 0U);
    auto wrong_parent = fact;
    wrong_parent.binding.parent.goal_id = id(3);
    write_work_authority_result(wrong_parent, bytes.data());
    std::vector<uint8_t> canonical(input_size);
    ASSERT_EQ(encode_approval_command(input, canonical.data(), input_size, input_size), AYNI_OK);
    auto accepted = mapping(1, id(1), fact.binding.established_seq);
    accepted.canonical_size = input_size;
    accepted.result_size = static_cast<uint32_t>(bytes.size());
    accepted.digest = authority_command_digest({canonical.data(), input_size});
    EXPECT_FALSE(valid_exact_mapping(accepted, {canonical.data(), input_size},
                                     {bytes.data(), accepted.result_size}));
}

// ADR-0038: type9/v5 retains both exact request and original result. Every
// truncated or extended body must preserve all caller outputs atomically.
TEST(WorkAuthorityResult, command_record_round_trip_and_all_truncations_are_atomic) {
    const auto request = approval(id(1));
    uint32_t input_size = 0;
    ASSERT_EQ(measure_approval_command(request, input_size), AYNI_OK);
    std::vector<uint8_t> canonical(input_size);
    ASSERT_EQ(encode_approval_command(request, canonical.data(), input_size, input_size), AYNI_OK);
    auto fact = result(AYNI_GOAL_REQUIREMENT_CAPACITY_MAX);
    fact.binding.input = {};
    std::vector<uint8_t> evidence(work_authority_result_size(fact.count));
    write_work_authority_result(fact, evidence.data());
    auto accepted = mapping(1, id(1), fact.binding.established_seq);
    accepted.canonical_size = input_size;
    accepted.result_size = static_cast<uint32_t>(evidence.size());
    accepted.digest = authority_command_digest({canonical.data(), input_size});
    const auto size = command_record_size_for(accepted);
    ASSERT_EQ(command_record_version_for(accepted), work_authority_command_record_version);
    std::vector<uint8_t> body(size + 1U);
    ASSERT_TRUE(write_command_record(accepted, 0, {canonical.data(), input_size}, body.data(), size,
                                     {evidence.data(), accepted.result_size}));
    CommandMapping decoded{};
    uint64_t floor = 99;
    ayni_bytes_v1 input{};
    ayni_bytes_v1 output{};
    ASSERT_TRUE(read_command_record(work_authority_command_record_version, body.data(), size,
                                    decoded, floor, input, &output));
    EXPECT_EQ(floor, 0U);
    EXPECT_EQ(decoded.object.id, id(1));
    EXPECT_EQ(decoded.result_size, evidence.size());
    EXPECT_EQ(std::vector<uint8_t>(input.data, input.data + input.size), canonical);
    EXPECT_EQ(std::vector<uint8_t>(output.data, output.data + output.size), evidence);
    for (uint32_t cut = 0; cut <= body.size(); ++cut) {
        if (cut == size) {
            continue;
        }
        auto untouched = accepted;
        uint64_t sentinel_floor = 99;
        ayni_bytes_v1 sentinel_input{canonical.data(), input_size};
        ayni_bytes_v1 sentinel_output{evidence.data(), accepted.result_size};
        EXPECT_FALSE(read_command_record(work_authority_command_record_version, body.data(), cut,
                                         untouched, sentinel_floor, sentinel_input,
                                         &sentinel_output));
        EXPECT_EQ(untouched.commit_seq, accepted.commit_seq);
        EXPECT_EQ(untouched.result_size, accepted.result_size);
        EXPECT_EQ(sentinel_floor, 99U);
        EXPECT_EQ(sentinel_input.data, canonical.data());
        EXPECT_EQ(sentinel_output.data, evidence.data());
    }
    const ayni::durability::RecordHeader old{command_record_type, approval_command_record_version,
                                             ayni::durability::record_required, size};
    EXPECT_EQ(WorkReplay::classify_header(old), ayni::durability::RecordUse::unknown);
}

// ADR-0038: command evidence has bounded historical IDs/stamps, including
// when no current Goal survives to provide an additional consistency check.
TEST(WorkAuthorityResult, historical_bounds_and_active_revision_apply_without_a_target) {
    auto fact = result(1);
    EXPECT_TRUE(work_authority_result_within(fact, fact.transition_id, 1));
    EXPECT_FALSE(work_authority_result_within(fact, fact.transition_id - 1U, 1));
    fact.binding.input.incarnation = 2;
    EXPECT_FALSE(work_authority_result_within(fact, fact.transition_id, 1));
    fact.binding.input.incarnation = 1;
    ayni::foundation::IdAllocator future;
    future.start(2);
    ASSERT_TRUE(future.allocate(fact.references[0].grant_id));
    EXPECT_FALSE(work_authority_result_within(fact, fact.transition_id, 1));
    fact = result(1);
    fact.references[0].revision = 123;
    EXPECT_FALSE(valid_work_authority_result(fact, id(1)));
}

// ADR-0038: later pauses retain the original binding result; while the current
// stamp still is the establishment stamp, its transition identity must match.
TEST(WorkAuthorityResult, current_generation_result_keeps_its_establishment_stamp) {
    auto fact = result(0);
    fact.binding.input = {};
    ayni::work::GoalAuthorityRecord current{};
    current.goal_id = id(1);
    current.binding = fact.binding;
    current.stamp = {fact.transition_id, fact.binding.established_seq};
    EXPECT_TRUE(retains_work_authority_result(current, fact));
    --fact.transition_id;
    EXPECT_FALSE(retains_work_authority_result(current, fact));
    ++current.stamp.commit_seq;
    ++current.stamp.transition_id;
    EXPECT_TRUE(retains_work_authority_result(current, fact));
    for (const auto transition : {current.stamp.transition_id, current.stamp.transition_id + 1U}) {
        fact.transition_id = transition;
        EXPECT_FALSE(retains_work_authority_result(current, fact));
    }
}

// ADR-0038: an accepted approval mapping always has encodable result facts.
// Missing results must not create a row, evict a command or advance its floor.
TEST(WorkAuthorityResult, missing_required_result_refuses_insert_and_restore) {
    constexpr uint32_t stride = 512;
    std::vector<uint64_t> arena(CommandTable::storage_size({1, 1, stride}) / sizeof(uint64_t) + 1U);
    CommandTable commands(arena.data(), {1, 1, stride});
    const auto request = approval(id(1));
    auto accepted = mapping(1, id(1), 42);
    ASSERT_EQ(commands.find_exact(77, 1, request).lookup, CommandTable::Lookup::unmapped);
    EXPECT_FALSE(commands.insert_exact(accepted));
    EXPECT_EQ(commands.size(), 0U);
    EXPECT_EQ(commands.floor(77), 0U);
    uint32_t size = 0;
    ASSERT_EQ(measure_approval_command(request, size), AYNI_OK);
    std::vector<uint8_t> canonical(size);
    ASSERT_EQ(encode_approval_command(request, canonical.data(), size, size), AYNI_OK);
    accepted.canonical_size = size;
    accepted.digest = authority_command_digest({canonical.data(), size});
    EXPECT_EQ(commands.restore_checked(accepted, 0, {canonical.data(), size}),
              CommandTable::Restore::corrupt);
    EXPECT_EQ(commands.size(), 0U);
    EXPECT_EQ(commands.floor(77), 0U);
}

// ADR-0038: the envelope identifies the result's establishing commit, even
// when no current Goal exists to supply another check of that relationship.
TEST(WorkAuthorityResult, enclosing_mapping_sequence_must_match_the_saved_binding) {
    const auto request = approval(id(1));
    uint32_t size = 0;
    ASSERT_EQ(measure_approval_command(request, size), AYNI_OK);
    std::vector<uint8_t> canonical(size);
    ASSERT_EQ(encode_approval_command(request, canonical.data(), size, size), AYNI_OK);
    auto fact = result(1);
    fact.binding.input = {};
    std::vector<uint8_t> evidence(work_authority_result_size(fact.count));
    write_work_authority_result(fact, evidence.data());
    auto accepted = mapping(1, id(1), fact.binding.established_seq + 1U);
    accepted.canonical_size = size;
    accepted.result_size = static_cast<uint32_t>(evidence.size());
    accepted.digest = authority_command_digest({canonical.data(), size});
    constexpr uint32_t stride = 512;
    std::vector<uint64_t> arena(CommandTable::storage_size({1, 1, stride}) / sizeof(uint64_t) + 1U);
    CommandTable commands(arena.data(), {1, 1, stride});
    EXPECT_EQ(commands.restore_checked(accepted, 0, {canonical.data(), size},
                                       {evidence.data(), accepted.result_size}),
              CommandTable::Restore::corrupt);
    EXPECT_EQ(commands.size(), 0U);
    EXPECT_EQ(commands.floor(77), 0U);
    accepted.commit_seq = fact.binding.established_seq;
    EXPECT_EQ(commands.restore_checked(accepted, 0, {canonical.data(), size},
                                       {evidence.data(), accepted.result_size}),
              CommandTable::Restore::restored);
}

// ADR-0038: the shared creation echo preserves delay and interval timing as
// distinct original inputs, independent of current catalog or live objects.
TEST(WorkAuthorityResult, shared_creation_echo_keeps_distinct_nonzero_delay_and_period) {
    for (const auto kind : {AYNI_TRIGGER_DELAY, AYNI_TRIGGER_INTERVAL}) {
        auto request = approval(id(1));
        auto &original = request.approval.original;
        original.trigger.kind = kind;
        original.trigger.delay_us = 17;
        original.trigger.period_us =
            kind == AYNI_TRIGGER_INTERVAL ? AYNI_TRIGGER_PERIOD_MIN_US : 0U;
        original.expiry_us = 99;
        uint32_t size = 0;
        ASSERT_EQ(measure_approval_command(request, size), AYNI_OK);
        std::vector<uint8_t> canonical(size);
        ASSERT_EQ(encode_approval_command(request, canonical.data(), size, size), AYNI_OK);
        ApprovalCommand decoded{};
        ASSERT_TRUE(read_approval_command({canonical.data(), size}, decoded));
        EXPECT_EQ(decoded.approval.original.trigger.kind, kind);
        EXPECT_EQ(decoded.approval.original.trigger.delay_us, 17U);
        EXPECT_EQ(decoded.approval.original.trigger.period_us, original.trigger.period_us);
        EXPECT_EQ(decoded.approval.original.expiry_us, 99U);
    }
}

} // namespace
