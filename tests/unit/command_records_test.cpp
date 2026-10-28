// ADR-0030: ordinary v1 remains readable; auth v2 must carry exact identity.
#include "foundation/kernel_id.h"
#include "foundation/little_endian.h"
#include "kernel/service/command/command_memory.h"
#include "kernel/service/command/command_records.h"

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

struct ExactManagementResult {
    CommandMapping mapping;
    std::vector<uint8_t> canonical;
};

// Produce requests through their actual domain codec; the expected result is
// supplied separately so a test cannot inherit the validator's revision formula.
template <typename Request>
ExactManagementResult management_result(const Request &request, ayni_object_kind_t object,
                                        uint64_t id, uint32_t revision,
                                        ayni_intent_effect_t effect = AYNI_INTENT_EFFECT_CHANGED) {
    uint32_t size = 0;
    EXPECT_EQ(ExactCommand<Request>::measure(request, size), AYNI_OK);
    std::vector<uint8_t> bytes(size);
    uint32_t written = 0;
    EXPECT_EQ(ExactCommand<Request>::encode(request, bytes.data(), size, written), AYNI_OK);
    EXPECT_EQ(written, size);
    CommandMapping mapping{1,
                           2,
                           authority_command_digest(view(bytes)),
                           9,
                           ExactCommand<Request>::kind(request),
                           {0, id, revision, object},
                           size};
    mapping.effect = effect;
    return {mapping, std::move(bytes)};
}

// C-WIRE-017 / C-STORE-009: an exact receipt must prove the complete request
// kind, local target and result revision. Recomputed digests cannot retarget it.
TEST(CommandRecords, management_results_require_their_exact_kind_and_revision_contract) {
    ayni::foundation::IdAllocator ids;
    ids.start(1);
    uint64_t actor = 0, context = 0, task = 0, call = 0, scene_id = 0, group = 0, memory = 0;
    for (auto *id : {&actor, &context, &task, &call, &scene_id, &group, &memory}) {
        ASSERT_TRUE(ids.allocate(*id));
    }
    ayni::catalog::CatalogId scene;
    ASSERT_TRUE(scene.assign({"look", 4}));
    using ayni::knowledge::ContextAction;
    MemoryCommand save{};
    save.kind = AYNI_INTENT_MEMORY_SAVE;
    save.value.space = ayni::knowledge::MemoryNamespace::private_agent;
    save.value.namespace_id = actor;
    save.value.source = ayni::knowledge::MemorySource::user;
    save.value.confirmed = true;
    save.value.body = {"explicit", 8};
    auto update = save;
    update.target_id = memory;
    update.expected_revision = 2;
    const ExactManagementResult examples[] = {
        management_result(TaskLimitCommand{2, task, 12}, AYNI_OBJECT_TASK, task, 3),
        management_result(TargetCommand{AYNI_INTENT_CALL_QUERY_RESULT, 2, call}, AYNI_OBJECT_CALL,
                          call, 3),
        management_result(TargetCommand{AYNI_INTENT_TASK_RESUME_WAIT, 2, task}, AYNI_OBJECT_TASK,
                          task, 3),
        management_result(ayni::work::PauseGroupChange{1, 2, true}, AYNI_OBJECT_PAUSE_GROUP, group,
                          3),
        management_result(
            ayni::authority::SceneChange{scene, 1, 2, ayni::authority::SceneMode::enabled},
            AYNI_OBJECT_SCENE, scene_id, 3),
        management_result(
            ayni::authority::SceneChange{scene, 1, 2, ayni::authority::SceneMode::enabled},
            AYNI_OBJECT_SCENE, scene_id, 2, AYNI_INTENT_EFFECT_NO_CHANGE),
        management_result(ContextCommand{AYNI_INTENT_CONTEXT_OPEN, 0, actor}, AYNI_OBJECT_CONTEXT,
                          context, 1),
        management_result(ContextCommand{AYNI_INTENT_CONTEXT_PUBLISH, 2, 0, context, 1, task, 2},
                          AYNI_OBJECT_CONTEXT, context, 2),
        management_result(ContextCommand{AYNI_INTENT_CONTEXT_FORGET, 2, 0, context, 1, 0, 0,
                                         ContextAction::erase_all},
                          AYNI_OBJECT_CONTEXT, context, 3),
        management_result(ContextCommand{AYNI_INTENT_CONTEXT_ACK, 2, 0, context, 1},
                          AYNI_OBJECT_CONTEXT, context, 2),
        management_result(save, AYNI_OBJECT_MEMORY, memory, 1),
        management_result(update, AYNI_OBJECT_MEMORY, memory, 3),
        management_result(MemoryCommand{AYNI_INTENT_MEMORY_CONFIRM, 2, memory}, AYNI_OBJECT_MEMORY,
                          memory, 3),
        management_result(MemoryCommand{AYNI_INTENT_MEMORY_DELETE, 2, memory}, AYNI_OBJECT_MEMORY,
                          memory, 3),
    };
    const auto changes = {
        +[](CommandMapping &v) { v.principal_id = 0; },
        +[](CommandMapping &v) { v.command_id = 0; },
        +[](CommandMapping &v) { v.object.kind = 0; },
        +[](CommandMapping &v) { v.object.id = 0; },
        +[](CommandMapping &v) { v.object.origin_device_id = 1; },
        +[](CommandMapping &v) { v.object.revision = 0; },
        +[](CommandMapping &v) { ++v.object.revision; },
        +[](CommandMapping &v) { ++v.canonical_size; },
        +[](CommandMapping &v) { ++v.result_size; },
        +[](CommandMapping &v) { v.effect = AYNI_INTENT_EFFECT_TARGET_GONE; },
        +[](CommandMapping &v) { v.privacy_expired = true; },
        +[](CommandMapping &v) { v.digest ^= 1; },
    };
    for (const auto &example : examples) {
        SCOPED_TRACE(example.mapping.kind);
        ASSERT_TRUE(valid_exact_mapping(example.mapping, view(example.canonical)));
        for (const auto change : changes) {
            auto candidate = example.mapping;
            change(candidate);
            EXPECT_FALSE(valid_exact_mapping(candidate, view(example.canonical)));
        }
        for (const auto &other : examples) {
            if (other.mapping.kind == example.mapping.kind) {
                continue;
            }
            auto candidate = example.mapping;
            candidate.canonical_size = static_cast<uint32_t>(other.canonical.size());
            candidate.digest = authority_command_digest(view(other.canonical));
            EXPECT_FALSE(valid_exact_mapping(candidate, view(other.canonical)));
        }
    }
}

// C-MEM-008 / C-STORE-007: privacy expiry retains only historical identity;
// private bytes, digest, sources and non-content command kinds cannot hide in it.
TEST(CommandRecords, expired_receipt_syntax_rejects_retained_private_or_foreign_identity) {
    ayni::foundation::IdAllocator ids;
    ids.start(1);
    uint64_t memory = 0;
    ASSERT_TRUE(ids.allocate(memory));
    CommandMapping valid{1, 2, 0, 9, AYNI_INTENT_MEMORY_SAVE, {0, memory, 1, AYNI_OBJECT_MEMORY}};
    valid.privacy_expired = true;
    ASSERT_TRUE(valid_expired_mapping(valid));
    const auto changes = {
        +[](CommandMapping &v) { v.kind = 0; },
        +[](CommandMapping &v) { v.privacy_expired = false; },
        +[](CommandMapping &v) { v.principal_id = 0; },
        +[](CommandMapping &v) { v.command_id = 0; },
        +[](CommandMapping &v) { v.digest = 1; },
        +[](CommandMapping &v) { v.canonical_size = 1; },
        +[](CommandMapping &v) { v.result_size = 1; },
        +[](CommandMapping &v) { v.sources.count = 1; },
        +[](CommandMapping &v) { v.object.origin_device_id = 1; },
        +[](CommandMapping &v) { v.object.id = 0; },
        +[](CommandMapping &v) { v.object.revision = 0; },
        +[](CommandMapping &v) { v.object.kind = AYNI_OBJECT_GOAL; },
        +[](CommandMapping &v) { v.effect = AYNI_INTENT_EFFECT_NO_CHANGE; },
        +[](CommandMapping &v) { v.retention = CommandRetention::accepted_revoke; },
    };
    for (const auto change : changes) {
        auto candidate = valid;
        change(candidate);
        EXPECT_FALSE(valid_expired_mapping(candidate));
    }
    for (const auto kind :
         {AYNI_INTENT_GOAL_CREATE, AYNI_INTENT_GOAL_APPROVE, AYNI_INTENT_GOAL_REAUTHORIZE,
          AYNI_INTENT_MEMORY_CONFIRM, AYNI_INTENT_MEMORY_DELETE}) {
        auto candidate = valid;
        candidate.kind = kind;
        candidate.object.kind =
            kind == AYNI_INTENT_MEMORY_CONFIRM || kind == AYNI_INTENT_MEMORY_DELETE
                ? AYNI_OBJECT_MEMORY
                : AYNI_OBJECT_GOAL;
        EXPECT_TRUE(valid_expired_mapping(candidate));
    }
    for (const auto kind : {AYNI_INTENT_GRANT_CREATE, AYNI_INTENT_TASK_RAISE_MODEL_LIMIT,
                            AYNI_INTENT_CALL_QUERY_RESULT, AYNI_INTENT_PAUSE_GROUP_SET,
                            AYNI_INTENT_SCENE_SET_ENABLED}) {
        auto candidate = valid;
        candidate.kind = kind;
        candidate.object.kind = intent_traits(kind).object;
        EXPECT_FALSE(valid_expired_mapping(candidate));
    }
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
