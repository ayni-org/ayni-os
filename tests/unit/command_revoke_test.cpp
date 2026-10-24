// ADR-0031: independent revoke identity and effect-bearing command v3.
#include "foundation/little_endian.h"
#include "kernel/command/command_records.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

namespace {
using namespace ayni::kernel;

std::array<uint8_t, 16> revoke_bytes(uint64_t target = 99, uint32_t expected = 1) {
    const ayni_grant_revoke_v1 body{sizeof(body), 0, target};
    std::array<uint8_t, 16> bytes{};
    uint32_t size = 0;
    EXPECT_EQ(encode_revoke_command({expected, &body}, bytes.data(), 16, size), AYNI_OK);
    EXPECT_EQ(size, 16U);
    return bytes;
}
ayni_bytes_v1 view(const std::array<uint8_t, 16> &bytes) {
    return {bytes.data(), static_cast<uint32_t>(bytes.size())};
}
CommandMapping revoke_mapping(const std::array<uint8_t, 16> &bytes,
                              ayni_intent_effect_t effect = AYNI_INTENT_EFFECT_CHANGED) {
    return {7,
            5,
            authority_command_digest(view(bytes)),
            9,
            AYNI_INTENT_GRANT_REVOKE,
            {0, 99, effect == AYNI_INTENT_EFFECT_TARGET_GONE ? 0U : 2U, AYNI_OBJECT_GRANT},
            16,
            effect};
}

// C-WIRE-017 / ADR-0031: exact revoke identity and versioned first-result encoding.
TEST(CommandRevoke, canonical_has_a_fixed_independent_little_endian_oracle) {
    const auto bytes = revoke_bytes(0x0807060504030201ULL, 0x01020304U);
    const std::array<uint8_t, 16> expected{10, 0, 0, 0, 4, 3, 2, 1, 1, 2, 3, 4, 5, 6, 7, 8};
    EXPECT_EQ(bytes, expected);
    RevokeCommandRecord restored{};
    ASSERT_TRUE(read_revoke_command(view(bytes), restored));
    EXPECT_EQ(restored.expected_revision, 0x01020304U);
    EXPECT_EQ(restored.target_grant_id, 0x0807060504030201ULL);
    EXPECT_NE(revoke_bytes(99, 1), revoke_bytes(99, 2));
    EXPECT_NE(revoke_bytes(99, 1), revoke_bytes(100, 1));
}

// C-WIRE-017 / ADR-0031: exact revoke identity and versioned first-result encoding.
TEST(CommandRevoke, malformed_typed_input_and_output_capacity_are_atomic) {
    ayni_grant_revoke_v1 body{sizeof(body), 0, 99};
    std::array<uint8_t, 20> bytes{};
    bytes.fill(0xA5);
    const auto before = bytes;
    uint32_t size = 123;
    const auto refuses = [&](CanonicalRevokeView input, ayni_status_t status) {
        EXPECT_EQ(encode_revoke_command(input, bytes.data(), 20, size), status);
        EXPECT_EQ(size, 123U);
        EXPECT_EQ(bytes, before);
    };
    refuses({1, nullptr}, AYNI_INVALID_ARGUMENT);
    --body.struct_size;
    refuses({1, &body}, AYNI_INVALID_ARGUMENT);
    body.struct_size += 2;
    refuses({1, &body}, AYNI_UNSUPPORTED_VERSION);
    --body.struct_size;
    body.reserved = 1;
    refuses({1, &body}, AYNI_INVALID_ARGUMENT);
    body.reserved = 0;
    body.grant_id = 0;
    refuses({1, &body}, AYNI_INVALID_ARGUMENT);
    body.grant_id = 99;
    refuses({0, &body}, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(encode_revoke_command({1, &body}, nullptr, 1, size), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(size, 123U);
    EXPECT_EQ(encode_revoke_command({1, &body}, bytes.data(), 15, size), AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(size, 16U);
    EXPECT_EQ(bytes, before);
    EXPECT_EQ(encode_revoke_command({1, &body}, nullptr, 0, size), AYNI_BUFFER_TOO_SMALL);
    ASSERT_EQ(encode_revoke_command({1, &body}, bytes.data(), 20, size), AYNI_OK);
    for (uint32_t index = 16; index < 20; ++index) {
        EXPECT_EQ(bytes[index], 0xA5);
    }
}

// C-WIRE-017 / ADR-0031: exact revoke identity and versioned first-result encoding.
TEST(CommandRevoke, complete_reader_rejects_truncation_tail_wrong_kind_and_zero_identity) {
    const auto original = revoke_bytes();
    RevokeCommandRecord out{123, 456};
    for (uint32_t length = 0; length < 16; ++length) {
        EXPECT_FALSE(read_revoke_command({original.data(), length}, out)) << length;
        EXPECT_EQ(out.expected_revision, 123U);
        EXPECT_EQ(out.target_grant_id, 456U);
    }
    std::vector<uint8_t> tail(original.begin(), original.end());
    tail.push_back(0);
    EXPECT_FALSE(read_revoke_command({tail.data(), static_cast<uint32_t>(tail.size())}, out));
    for (uint32_t at : {0U, 4U, 8U}) {
        auto bad = original;
        bad[at] = 0;
        EXPECT_FALSE(read_revoke_command(view(bad), out)) << at;
    }
    EXPECT_FALSE(read_revoke_command({nullptr, 16}, out));
    EXPECT_EQ(out.expected_revision, 123U);
    EXPECT_EQ(out.target_grant_id, 456U);
}

// C-WIRE-017 / ADR-0031: exact revoke identity and versioned first-result encoding.
TEST(CommandRevoke, v3_stores_explicit_effect_and_original_identity_without_retention) {
    const auto canonical = revoke_bytes();
    for (const auto effect : {AYNI_INTENT_EFFECT_CHANGED, AYNI_INTENT_EFFECT_NO_CHANGE,
                              AYNI_INTENT_EFFECT_TARGET_GONE}) {
        SCOPED_TRACE(effect);
        auto mapping = revoke_mapping(canonical, effect);
        mapping.retention = CommandRetention::accepted_revoke;
        std::array<uint8_t, 80> body{};
        ASSERT_TRUE(write_command_record(mapping, 4, view(canonical), body.data(), 80));
        EXPECT_EQ(command_record_version_for(mapping), 3U);
        EXPECT_EQ(command_record_size_for(mapping), 80U);
        // Contract v3 follows the old 52-byte fixed result with effect,
        // canonical format, length, then the complete fixed16 identity.
        EXPECT_EQ(ayni::foundation::load_le32(body.data() + 52), effect);
        const std::array<uint8_t, 8> format_and_size{1, 0, 0, 0, 16, 0, 0, 0};
        EXPECT_EQ(std::memcmp(body.data() + 56, format_and_size.data(), 8), 0);
        EXPECT_EQ(std::memcmp(body.data() + 64, canonical.data(), 16), 0);
        CommandMapping restored{};
        uint64_t floor = 0;
        ayni_bytes_v1 exact{};
        ASSERT_TRUE(read_command_record(3, body.data(), 80, restored, floor, exact));
        EXPECT_EQ(restored.effect, effect);
        EXPECT_EQ(restored.object.revision, mapping.object.revision);
        EXPECT_EQ(restored.commit_seq, 0U);
        EXPECT_EQ(restored.retention, CommandRetention::normal);
        EXPECT_EQ(floor, 4U);
        EXPECT_EQ(exact.data, body.data() + 64);
        EXPECT_EQ(exact.size, 16U);
    }
    EXPECT_EQ(command_record_max(0), 52U);
    EXPECT_EQ(command_record_max(16), 80U);
    EXPECT_EQ(command_record_max(340), 404U);
}

// C-WIRE-017 / ADR-0031: exact revoke identity and versioned first-result encoding.
TEST(CommandRevoke, v3_damage_or_cross_version_interpretation_cannot_publish_partial_outputs) {
    const auto canonical = revoke_bytes();
    const auto mapping = revoke_mapping(canonical);
    std::array<uint8_t, 80> body{};
    ASSERT_TRUE(write_command_record(mapping, 4, view(canonical), body.data(), 80));
    CommandMapping restored{};
    restored.principal_id = 123;
    uint64_t floor = 456;
    ayni_bytes_v1 exact{body.data(), 789};
    const auto refuses = [&](const uint8_t *data, uint32_t size, uint16_t version = 3) {
        EXPECT_FALSE(read_command_record(version, data, size, restored, floor, exact));
        EXPECT_EQ(restored.principal_id, 123U);
        EXPECT_EQ(floor, 456U);
        EXPECT_EQ(exact.data, body.data());
        EXPECT_EQ(exact.size, 789U);
    };
    for (uint32_t size = 0; size < 80; ++size) {
        refuses(body.data(), size);
    }
    for (uint32_t at : {32U, 36U, 40U, 48U, 52U, 56U, 60U, 64U, 68U, 72U}) {
        auto damaged = body;
        damaged[at] = 0;
        refuses(damaged.data(), 80);
    }
    auto damaged = body;
    damaged[52] = 4; // Unknown effect.
    refuses(damaged.data(), 80);
    refuses(body.data(), 80, 1);
    refuses(body.data(), 80, 2);
    refuses(body.data(), 80, 4);
    const auto old_v2 = [&] {
        std::vector<uint8_t> value(body.begin(), body.end());
        value.erase(value.begin() + 52, value.begin() + 56); // Strip effect like v2.
        return value;
    }();
    refuses(old_v2.data(), static_cast<uint32_t>(old_v2.size()), 2);
}

// C-WIRE-017 / ADR-0031: exact revoke identity and versioned first-result encoding.
TEST(CommandRevoke, result_effect_and_target_are_validated_before_writing) {
    const auto canonical = revoke_bytes();
    std::array<uint8_t, 80> bytes{};
    bytes.fill(0xA5);
    const auto before = bytes;
    const auto refuses = [&](CommandMapping mapping) {
        EXPECT_FALSE(write_command_record(mapping, 0, view(canonical), bytes.data(), 80));
        EXPECT_EQ(bytes, before);
    };
    auto mapping = revoke_mapping(canonical);
    mapping.object.id = 100;
    refuses(mapping);
    mapping = revoke_mapping(canonical);
    mapping.object.origin_device_id = 1;
    refuses(mapping);
    mapping = revoke_mapping(canonical);
    mapping.effect = AYNI_INTENT_EFFECT_NONE;
    refuses(mapping);
    for (const auto effect : {AYNI_INTENT_EFFECT_CHANGED, AYNI_INTENT_EFFECT_NO_CHANGE}) {
        mapping = revoke_mapping(canonical, effect);
        mapping.object.revision = 0;
        refuses(mapping);
    }
    mapping = revoke_mapping(canonical, AYNI_INTENT_EFFECT_TARGET_GONE);
    mapping.object.revision = 2;
    refuses(mapping);
}

// C-WIRE-017 / ADR-0031: exact revoke identity and versioned first-result encoding.
TEST(CommandRevoke, accepted_revision_two_can_change_a_restored_active_target_after_reload) {
    // First acceptance saw a child at volatile REVOKED/revision2. An
    // uncommitted ancestor cleanup can disappear on reload, restoring
    // ACTIVE/revision1; the accepted request retains its original CAS.
    const auto canonical = revoke_bytes(99, 2);
    const auto mapping = revoke_mapping(canonical, AYNI_INTENT_EFFECT_CHANGED);
    std::array<uint8_t, 80> bytes{};
    ASSERT_TRUE(write_command_record(mapping, 0, view(canonical), bytes.data(), 80));
    CommandMapping restored{};
    uint64_t floor = 0;
    ayni_bytes_v1 exact{};
    ASSERT_TRUE(read_command_record(3, bytes.data(), 80, restored, floor, exact));
    RevokeCommandRecord request{};
    ASSERT_TRUE(read_revoke_command(exact, request));
    EXPECT_EQ(request.expected_revision, 2U);
    EXPECT_EQ(restored.effect, AYNI_INTENT_EFFECT_CHANGED);
    EXPECT_EQ(restored.object.revision, 2U);

    std::vector<uint64_t> storage(
        CommandTable::storage_size(1, 1, revoke_command_size) / sizeof(uint64_t) + 1U);
    CommandTable table(storage.data(), 1, 1, revoke_command_size);
    EXPECT_EQ(table.restore_checked(mapping, 0, view(canonical)), CommandTable::Restore::restored);
    ASSERT_EQ(table.size(), 1U);
    EXPECT_EQ(table.at(0).object.revision, 2U);
    EXPECT_EQ(table.at(0).effect, AYNI_INTENT_EFFECT_CHANGED);
}

// C-WIRE-017 / ADR-0031: exact revoke identity and versioned first-result encoding.
TEST(CommandRevoke, impossible_accepted_revision_is_rejected_atomically_for_every_effect) {
    // Shape-valid raw requests must reach fresh API CAS validation. Only
    // accepted mappings narrow the original revision to the published 1/2 states.
    const auto invalid_canonical = revoke_bytes(99, 999);
    RevokeCommandRecord raw{};
    ASSERT_TRUE(read_revoke_command(view(invalid_canonical), raw));
    EXPECT_EQ(raw.expected_revision, 999U);
    constexpr uint32_t digest_offset = 2U * sizeof(uint64_t);
    constexpr uint32_t record_size = revoke_command_record_fixed_size + revoke_command_size;
    const auto valid_canonical = revoke_bytes(99, 2);
    for (const auto effect : {AYNI_INTENT_EFFECT_CHANGED, AYNI_INTENT_EFFECT_NO_CHANGE,
                              AYNI_INTENT_EFFECT_TARGET_GONE}) {
        SCOPED_TRACE(effect);
        const auto invalid = revoke_mapping(invalid_canonical, effect);
        EXPECT_FALSE(valid_exact_mapping(invalid, view(invalid_canonical)));
        std::array<uint8_t, record_size> output{};
        output.fill(0xA5);
        const auto before_output = output;
        EXPECT_FALSE(
            write_command_record(invalid, 4, view(invalid_canonical), output.data(), record_size));
        EXPECT_EQ(output, before_output);

        const auto valid = revoke_mapping(valid_canonical, effect);
        std::array<uint8_t, record_size> damaged{};
        ASSERT_TRUE(
            write_command_record(valid, 4, view(valid_canonical), damaged.data(), record_size));
        std::memcpy(damaged.data() + revoke_command_record_fixed_size, invalid_canonical.data(),
                    revoke_command_size);
        ayni::foundation::store_le64(damaged.data() + digest_offset, invalid.digest);
        CommandMapping decoded = valid;
        uint64_t floor = 123;
        ayni_bytes_v1 exact{output.data(), 456};
        EXPECT_FALSE(read_command_record(revoke_command_record_version, damaged.data(), record_size,
                                         decoded, floor, exact));
        EXPECT_EQ(decoded.principal_id, valid.principal_id);
        EXPECT_EQ(decoded.command_id, valid.command_id);
        EXPECT_EQ(decoded.digest, valid.digest);
        EXPECT_EQ(decoded.commit_seq, valid.commit_seq);
        EXPECT_EQ(decoded.kind, valid.kind);
        EXPECT_EQ(decoded.canonical_size, valid.canonical_size);
        EXPECT_EQ(decoded.effect, valid.effect);
        EXPECT_EQ(decoded.retention, valid.retention);
        EXPECT_EQ(decoded.object.origin_device_id, valid.object.origin_device_id);
        EXPECT_EQ(decoded.object.kind, valid.object.kind);
        EXPECT_EQ(decoded.object.id, valid.object.id);
        EXPECT_EQ(decoded.object.revision, valid.object.revision);
        EXPECT_EQ(floor, 123U);
        EXPECT_EQ(exact.data, output.data());
        EXPECT_EQ(exact.size, 456U);

        std::vector<uint64_t> storage(
            CommandTable::storage_size(1, 2, revoke_command_size) / sizeof(uint64_t) + 1U);
        CommandTable table(storage.data(), 1, 2, revoke_command_size);
        auto seed = valid;
        seed.command_id = 4;
        seed.commit_seq = 8;
        ASSERT_EQ(table.restore_checked(seed, 2, view(valid_canonical)),
                  CommandTable::Restore::restored);
        const auto before_storage = storage;
        EXPECT_EQ(table.restore_checked(invalid, 100, view(invalid_canonical)),
                  CommandTable::Restore::corrupt);
        EXPECT_EQ(storage, before_storage);
        EXPECT_EQ(table.size(), 1U);
        EXPECT_EQ(table.floor(7), 2U);
        EXPECT_EQ(table.at(0).command_id, 4U);
        const auto retained = table.canonical(table.at(0));
        ASSERT_EQ(retained.size, valid_canonical.size());
        EXPECT_EQ(std::memcmp(retained.data, valid_canonical.data(), retained.size), 0);
    }
}

// C-WIRE-017 / ADR-0031: exact revoke identity and versioned first-result encoding.
TEST(CommandRevoke, a_revoke_result_cannot_publish_a_nonfixed_canonical_size) {
    const auto canonical = revoke_bytes();
    auto mapping = revoke_mapping(canonical);
    constexpr uint32_t record_size = revoke_command_record_fixed_size + revoke_command_size;
    std::array<uint8_t, record_size> bytes{};
    bytes.fill(0xA5);
    const auto before = bytes;
    for (const auto size : {revoke_command_size - 1U, revoke_command_size + 1U}) {
        mapping.canonical_size = size;
        EXPECT_EQ(command_record_size_for(mapping), 0U);
        EXPECT_FALSE(write_command_record(mapping, 0, view(canonical), bytes.data(), record_size));
        EXPECT_EQ(bytes, before);
    }
}
} // namespace
