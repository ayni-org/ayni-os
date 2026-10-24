// ADR-0030: ordinary v1 remains readable; auth v2 must carry exact identity.
#include "foundation/little_endian.h"
#include "kernel/command/command_records.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace {
using namespace ayni::kernel;

std::vector<uint8_t> identity(uint32_t flags = 0) {
    ayni_grant_create_v1 grant{};
    grant.struct_size = sizeof(grant);
    grant.capability_version = 1;
    grant.capability_id = {"a", 1};
    grant.subject_instance_id = 1;
    grant.lifetime = AYNI_GRANT_PERMANENT;
    grant.flags = flags;
    grant.scope.struct_size = sizeof(grant.scope);
    uint32_t size = 0;
    EXPECT_EQ(measure_authority_command({0, &grant}, size), AYNI_OK);
    std::vector<uint8_t> bytes(size);
    EXPECT_EQ(encode_authority_command({0, &grant}, bytes.data(), size, size), AYNI_OK);
    return bytes;
}
ayni_bytes_v1 view(const std::vector<uint8_t> &bytes) {
    return {bytes.data(), static_cast<uint32_t>(bytes.size())};
}
CommandMapping auth_mapping(const std::vector<uint8_t> &bytes) {
    return {1,
            2,
            authority_command_digest(view(bytes)),
            9,
            AYNI_INTENT_GRANT_CREATE,
            {0, 5, 1, AYNI_OBJECT_GRANT},
            static_cast<uint32_t>(bytes.size())};
}

TEST(CommandRecords, ordinary_version_one_bytes_and_original_result_remain_compatible) {
    const CommandMapping ordinary{1, 2, 3, 9, AYNI_INTENT_GOAL_CREATE, {0, 5, 6, AYNI_OBJECT_GOAL}};
    std::array<uint8_t, 52> bytes{};
    write_command_record(ordinary, 4, bytes.data());
    const std::array<uint8_t, 52> expected{1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 3, 0,
                                           0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
                                           9, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0, 6, 0, 0, 0};
    EXPECT_EQ(bytes, expected);
    CommandMapping restored{};
    uint64_t floor = 0;
    ASSERT_TRUE(
        read_command_record(bytes.data(), static_cast<uint32_t>(bytes.size()), restored, floor));
    EXPECT_EQ(restored.commit_seq, 0U);
    EXPECT_EQ(restored.canonical_size, 0U);
    EXPECT_EQ(restored.object.id, 5U);
    EXPECT_EQ(floor, 4U);
    EXPECT_EQ(command_record_version_for(ordinary), 1U);
}

TEST(CommandRecords, auth_version_two_round_trips_full_request_and_borrows_only_the_input_body) {
    const auto canonical = identity();
    const auto mapping = auth_mapping(canonical);
    std::vector<uint8_t> bytes(command_record_size_for(mapping));
    ASSERT_TRUE(write_command_record(mapping, 1, view(canonical), bytes.data(),
                                     static_cast<uint32_t>(bytes.size())));
    EXPECT_EQ(bytes.size(), 60U + canonical.size());
    EXPECT_EQ(command_record_version_for(mapping), 2U);
    EXPECT_EQ(command_record_max(340), 404U);
    CommandMapping restored{};
    uint64_t floor = 0;
    ayni_bytes_v1 restored_bytes{};
    ASSERT_TRUE(read_command_record(2, bytes.data(), static_cast<uint32_t>(bytes.size()), restored,
                                    floor, restored_bytes));
    EXPECT_EQ(restored.digest, mapping.digest);
    EXPECT_EQ(restored.canonical_size, canonical.size());
    EXPECT_EQ(restored.commit_seq, 0U);
    EXPECT_EQ(restored.object.kind, AYNI_OBJECT_GRANT);
    EXPECT_EQ(floor, 1U);
    EXPECT_EQ(
        (std::vector<uint8_t>{restored_bytes.data, restored_bytes.data + restored_bytes.size}),
        canonical);
    EXPECT_EQ(restored_bytes.data, bytes.data() + 60);
}

TEST(CommandRecords, every_truncated_or_altered_auth_identity_is_rejected_without_output_changes) {
    const auto canonical = identity();
    const auto mapping = auth_mapping(canonical);
    std::vector<uint8_t> bytes(command_record_size_for(mapping));
    ASSERT_TRUE(write_command_record(mapping, 1, view(canonical), bytes.data(),
                                     static_cast<uint32_t>(bytes.size())));
    CommandMapping restored{};
    restored.principal_id = 77;
    uint64_t floor = 88;
    const uint8_t sentinel = 0;
    ayni_bytes_v1 result{&sentinel, 99};
    for (uint32_t size = 0; size < bytes.size(); ++size) {
        EXPECT_FALSE(read_command_record(2, bytes.data(), size, restored, floor, result)) << size;
        EXPECT_EQ(restored.principal_id, 77U);
        EXPECT_EQ(floor, 88U);
        EXPECT_EQ(result.data, &sentinel);
        EXPECT_EQ(result.size, 99U);
    }
    auto damaged = bytes;
    damaged.back() ^= 1; // canonical bytes differ from recorded digest
    EXPECT_FALSE(read_command_record(2, damaged.data(), static_cast<uint32_t>(damaged.size()),
                                     restored, floor, result));
    damaged = bytes;
    damaged.push_back(0);
    EXPECT_FALSE(read_command_record(2, damaged.data(), static_cast<uint32_t>(damaged.size()),
                                     restored, floor, result));
    damaged = bytes;
    damaged[52] = 2; // unknown canonical format
    EXPECT_FALSE(read_command_record(2, damaged.data(), static_cast<uint32_t>(damaged.size()),
                                     restored, floor, result));
    EXPECT_FALSE(read_command_record(3, bytes.data(), static_cast<uint32_t>(bytes.size()), restored,
                                     floor, result));
    EXPECT_EQ(restored.principal_id, 77U);
}

TEST(CommandRecords,
     v1_auth_missing_identity_unsupported_flags_and_wrong_results_are_never_written) {
    auto canonical = identity();
    auto mapping = auth_mapping(canonical);
    std::array<uint8_t, 256> bytes{};
    bytes.fill(0xA5);
    const auto before = bytes;
    EXPECT_FALSE(write_command_record(mapping, 0, {}, bytes.data(), 52));
    EXPECT_EQ(bytes, before);
    ++mapping.digest;
    EXPECT_FALSE(write_command_record(mapping, 0, view(canonical), bytes.data(),
                                      command_record_size_for(mapping)));
    EXPECT_EQ(bytes, before);
    mapping = auth_mapping(canonical);
    mapping.object.kind = AYNI_OBJECT_GOAL;
    EXPECT_FALSE(write_command_record(mapping, 0, view(canonical), bytes.data(),
                                      command_record_size_for(mapping)));
    canonical = identity(AYNI_GRANT_REQUIRES_SCENE);
    mapping = auth_mapping(canonical);
    EXPECT_FALSE(write_command_record(mapping, 0, view(canonical), bytes.data(),
                                      command_record_size_for(mapping)));
    EXPECT_EQ(bytes, before);

    CommandMapping ordinary{1, 2, 3, 0, AYNI_INTENT_GOAL_CREATE, {0, 5, 1, AYNI_OBJECT_GOAL}};
    write_command_record(ordinary, 0, bytes.data());
    // White-box input damage: a prior reader could never have written kind9.
    ayni::foundation::store_le32(bytes.data() + 32, AYNI_INTENT_GRANT_CREATE);
    uint64_t floor = 0;
    EXPECT_FALSE(read_command_record(bytes.data(), 52, ordinary, floor));
}

TEST(CommandRecords, invalid_output_shape_or_ordinary_kind_leaves_the_complete_buffer_untouched) {
    CommandMapping ordinary{1, 2, 3, 0, AYNI_INTENT_GOAL_CREATE, {0, 5, 1, AYNI_OBJECT_GOAL}};
    std::array<uint8_t, 256> bytes{};
    bytes.fill(0xA5);
    const auto before = bytes;
    EXPECT_EQ(command_record_max(0), 52U);
    EXPECT_FALSE(write_command_record(ordinary, 0, {}, nullptr, 52));
    EXPECT_FALSE(write_command_record(ordinary, 0, {}, bytes.data(), 0));
    EXPECT_FALSE(write_command_record(ordinary, 0, {}, bytes.data(), 51));
    const auto canonical = identity();
    EXPECT_FALSE(write_command_record(ordinary, 0, view(canonical), bytes.data(), 52));
    for (const auto kind : {0U, AYNI_INTENT_INSTANCE_STOP + 1U}) {
        ordinary.kind = kind;
        EXPECT_FALSE(write_command_record(ordinary, 0, {}, bytes.data(), 52));
    }
    auto oversized = auth_mapping(canonical);
    oversized.canonical_size = 84U + 4096U + 1U;
    EXPECT_EQ(command_record_size_for(oversized), 0U);
    EXPECT_FALSE(write_command_record(oversized, 0, view(canonical), bytes.data(), 52));
    EXPECT_EQ(bytes, before);
}

TEST(CommandRecords, invalid_authority_result_or_identity_is_rejected_atomically_by_codec) {
    const auto canonical = identity();
    const auto original = auth_mapping(canonical);
    std::array<uint8_t, 256> bytes{};
    bytes.fill(0xA5);
    const auto before = bytes;
    const auto refuses = [&](const CommandMapping &mapping) {
        EXPECT_FALSE(write_command_record(mapping, 0, view(canonical), bytes.data(),
                                          command_record_size_for(mapping)));
        EXPECT_EQ(bytes, before);
    };
    EXPECT_FALSE(
        write_command_record(original, 0, {}, bytes.data(), command_record_size_for(original)));
    EXPECT_EQ(bytes, before);
    auto damaged = original;
    damaged.principal_id = 0;
    refuses(damaged);
    damaged = original;
    damaged.command_id = 0;
    refuses(damaged);
    damaged = original;
    damaged.kind = AYNI_INTENT_GOAL_CREATE;
    refuses(damaged);
    damaged = original;
    damaged.object.id = 0;
    refuses(damaged);
    for (const auto revision : {0U, 2U}) {
        damaged = original;
        damaged.object.revision = revision;
        refuses(damaged);
    }
    damaged = original;
    ++damaged.canonical_size;
    refuses(damaged);
    CommandMapping output{};
    output.principal_id = 777;
    uint64_t floor = 888;
    ayni_bytes_v1 restored{bytes.data(), 999};
    EXPECT_FALSE(read_command_record(1, nullptr, 52, output, floor, restored));
    EXPECT_EQ(output.principal_id, 777U);
    EXPECT_EQ(floor, 888U);
    EXPECT_EQ(restored.data, bytes.data());
    EXPECT_EQ(restored.size, 999U);
}
} // namespace
