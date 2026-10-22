#include "authority/grant/grant_records.h"
#include "authority/scope/scope.h"
#include "catalog/capability/field_domain.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {
using namespace ayni::authority;

constexpr uint64_t grant_id = UINT64_C(0x100000001);
constexpr std::array<uint8_t, 4> empty_domain{1, 0, 0, 0};
// Independent ADR-0030 v1 oracle: the empty-domain root has a one-byte ID.
constexpr std::array<uint8_t, 89> root_body{
    1, 0, 0, 0, 1,   0, 0, 0, // Grant ID
    0, 0, 0, 0, 0,   0, 0, 0, // replaced Grant
    2, 0, 0, 0, 0,   0, 0, 0, // subject
    0, 0, 0, 0, 0,   0, 0, 0, // parent
    3, 0, 0, 0, 0,   0, 0, 0, // owner
    4, 0, 0, 0, 5,   0, 0, 0, // binding revision, Capability version
    1, 0, 0, 0, 'x',          // Capability ID
    1, 0, 0, 0, 1,   0, 0, 0, // PERMANENT, allow delegation
    0, 0, 0, 0, 0,   0, 0, 0, // duration
    0, 0, 0, 0,               // depth
    4, 0, 0, 0, 1,   0, 0, 0, // source domain
    4, 0, 0, 0, 1,   0, 0, 0  // expanded scope
};

Grant root() {
    Grant grant{};
    grant.grant_id = grant_id;
    grant.subject_instance_id = 2;
    grant.owner_principal_id = 3;
    grant.owner_binding_revision = 4;
    EXPECT_TRUE(grant.capability_id.assign({"x", 1}));
    grant.capability_version = 5;
    grant.lifetime = AYNI_GRANT_PERMANENT;
    grant.allow_delegation = true;
    grant.created_stamp = {11, 12};
    grant.stamp = grant.created_stamp;
    grant.revision = 1;
    grant.lifecycle = AYNI_GRANT_ACTIVE;
    grant.source_domain_size = 4;
    grant.scope_size = 4;
    return grant;
}

ayni_bytes_v1 domain() {
    return {empty_domain.data(), static_cast<uint32_t>(empty_domain.size())};
}

TEST(GrantRecords, create_encoding_matches_independent_literal_and_borrows_only_record_bytes) {
    const auto grant = root();
    ASSERT_EQ(grant_record_size(grant), root_body.size());
    std::array<uint8_t, root_body.size()> bytes{};
    write_grant_record(grant, domain(), domain(), bytes.data());
    EXPECT_EQ(bytes, root_body);
    GrantRecord record{};
    ASSERT_TRUE(
        read_grant_record(root_body.data(), static_cast<uint32_t>(root_body.size()), record));
    EXPECT_EQ(record.grant_id, grant_id);
    EXPECT_EQ(record.grant.subject_instance_id, 2U);
    EXPECT_EQ(record.grant.owner_principal_id, 3U);
    EXPECT_EQ(record.grant.owner_binding_revision, 4U);
    EXPECT_EQ(record.grant.capability_version, 5U);
    EXPECT_TRUE(record.grant.capability_id.equals({"x", 1}));
    EXPECT_EQ(record.grant.flags, AYNI_GRANT_ALLOW_DELEGATION);
    EXPECT_EQ(record.grant.scope, nullptr);
    EXPECT_EQ(record.grant.source_domain.data, root_body.data() + 77U);
    EXPECT_EQ(record.scope.data, root_body.data() + 85U);
    EXPECT_TRUE(valid_grant_record(record));
}

TEST(GrantRecords, every_truncation_and_trailing_byte_is_rejected_without_publishing_output) {
    for (uint32_t size = 0; size < root_body.size(); ++size) {
        GrantRecord record{};
        record.grant_id = 91;
        EXPECT_FALSE(read_grant_record(root_body.data(), size, record)) << size;
        EXPECT_EQ(record.grant_id, 91U) << size;
    }
    std::vector<uint8_t> bytes(root_body.begin(), root_body.end());
    bytes.push_back(0);
    GrantRecord record{};
    EXPECT_FALSE(read_grant_record(bytes.data(), static_cast<uint32_t>(bytes.size()), record));
    EXPECT_FALSE(read_grant_record(nullptr, static_cast<uint32_t>(root_body.size()), record));
}

TEST(GrantRecords, malformed_id_flags_lifetime_depth_and_canonical_domains_are_rejected) {
    for (const uint32_t offset :
         {0U, 4U, 16U, 32U, 40U, 44U, 48U, 52U, 53U, 57U, 61U, 69U, 73U, 77U, 80U, 81U, 85U, 88U}) {
        auto bytes = root_body;
        // Zero is invalid for identities; the remaining cases break a tag,
        // size, reserved field, duration, or parent/depth relationship.
        bytes[offset] = offset <= 53U ? 0U : 2U;
        if (offset == 52U) {
            bytes[offset] = ' ';
        }
        GrantRecord record{};
        EXPECT_FALSE(read_grant_record(bytes.data(), static_cast<uint32_t>(bytes.size()), record))
            << offset;
    }
}

TEST(GrantRecords, duration_is_persistent_while_deadline_hold_and_transition_stamps_are_not) {
    auto grant = root();
    grant.lifetime = AYNI_GRANT_CURRENT_BOOT;
    grant.duration_us = UINT64_C(0x0102030405060708);
    auto expected = root_body;
    expected[53] = 2;
    expected[61] = 8;
    expected[62] = 7;
    expected[63] = 6;
    expected[64] = 5;
    expected[65] = 4;
    expected[66] = 3;
    expected[67] = 2;
    expected[68] = 1;
    std::array<uint8_t, root_body.size()> bytes{};
    write_grant_record(grant, domain(), domain(), bytes.data());
    EXPECT_EQ(bytes, expected);
    grant.deadline_us = UINT64_MAX;
    grant.hold_reason = AYNI_REASON_GRANT_SUBJECT;
    grant.stamp = {999, 1000};
    write_grant_record(grant, domain(), domain(), bytes.data());
    EXPECT_EQ(bytes, expected);
    GrantRecord decoded{};
    ASSERT_TRUE(read_grant_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded));
    EXPECT_EQ(decoded.grant.duration_us, UINT64_C(0x0102030405060708));
}

TEST(GrantRecords, state_has_one_legal_terminal_shape_and_exact_little_endian_bytes) {
    auto grant = root();
    grant.lifecycle = AYNI_GRANT_EXPIRED;
    grant.revision = 2;
    constexpr std::array<uint8_t, 16> expected{1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0};
    std::array<uint8_t, 16> bytes{};
    write_grant_state_record(grant, bytes.data());
    EXPECT_EQ(bytes, expected);
    GrantStateRecord state{};
    ASSERT_TRUE(read_grant_state_record(bytes.data(), static_cast<uint32_t>(bytes.size()), state));
    EXPECT_EQ(state.grant_id, grant_id);
    EXPECT_EQ(state.revision, 2U);
    for (const uint32_t offset : {0U, 4U, 8U, 12U}) {
        bytes = expected;
        bytes[offset] = 0;
        state.grant_id = 91;
        EXPECT_FALSE(
            read_grant_state_record(bytes.data(), static_cast<uint32_t>(bytes.size()), state));
        EXPECT_EQ(state.grant_id, 91U);
    }
    for (uint32_t size = 0; size < expected.size(); ++size) {
        EXPECT_FALSE(read_grant_state_record(expected.data(), size, state));
    }
    EXPECT_FALSE(read_grant_state_record(nullptr, 16, state));
}

TEST(GrantRecords, snapshot_adds_original_stamps_revision_and_state_without_a_deadline) {
    auto grant = root();
    ASSERT_EQ(grant_snapshot_record_size(grant), root_body.size() + 40U);
    std::vector<uint8_t> expected(root_body.begin(), root_body.end());
    constexpr std::array<uint8_t, 40> tail{11, 0, 0,  0, 0, 0, 0, 0, 12, 0, 0,  0, 0, 0,
                                           0,  0, 11, 0, 0, 0, 0, 0, 0,  0, 12, 0, 0, 0,
                                           0,  0, 0,  0, 1, 0, 0, 0, 1,  0, 0,  0};
    expected.insert(expected.end(), tail.begin(), tail.end());
    std::vector<uint8_t> bytes(expected.size());
    grant.deadline_us = UINT64_MAX;
    grant.hold_reason = AYNI_REASON_GRANT_CHANNEL;
    write_grant_snapshot_record(grant, domain(), domain(), bytes.data());
    EXPECT_EQ(bytes, expected);
    GrantSnapshotRecord record{};
    ASSERT_TRUE(
        read_grant_snapshot_record(bytes.data(), static_cast<uint32_t>(bytes.size()), record));
    EXPECT_EQ(record.created_stamp.transition_id, 11U);
    EXPECT_EQ(record.created_stamp.commit_seq, 12U);
    EXPECT_EQ(record.stamp.transition_id, 11U);
    EXPECT_EQ(record.stamp.commit_seq, 12U);
    EXPECT_EQ(record.revision, 1U);
    EXPECT_EQ(record.lifecycle, AYNI_GRANT_ACTIVE);
    for (uint32_t size = 0; size < expected.size(); ++size) {
        record.revision = 91;
        EXPECT_FALSE(read_grant_snapshot_record(bytes.data(), size, record)) << size;
        EXPECT_EQ(record.revision, 91U);
    }
    bytes.push_back(0);
    EXPECT_FALSE(
        read_grant_snapshot_record(bytes.data(), static_cast<uint32_t>(bytes.size()), record));
}

TEST(GrantRecords, fixed_budget_includes_all_three_length_prefixes_and_snapshot_tail) {
    EXPECT_EQ(grant_record_type, 13U);
    EXPECT_EQ(grant_state_record_type, 14U);
    EXPECT_EQ(grant_record_version, 1U);
    EXPECT_EQ(grant_state_record_version, 1U);
    EXPECT_EQ(grant_record_max(64), 240U);
    EXPECT_EQ(grant_record_max(4096), 8304U);
    EXPECT_EQ(grant_snapshot_record_max(64), 280U);
    EXPECT_EQ(grant_snapshot_record_max(4096), 8344U);
}

TEST(GrantRecords, restored_facts_validate_identity_lifetime_depth_and_both_canonical_budgets) {
    for (uint32_t fault = 0; fault < 16; ++fault) {
        GrantRecord record{};
        ASSERT_TRUE(
            read_grant_record(root_body.data(), static_cast<uint32_t>(root_body.size()), record));
        switch (fault) {
        case 0:
            record.grant.owner_binding_revision = 0;
            break;
        case 1:
            record.grant.flags = AYNI_GRANT_REQUIRES_SCENE;
            break;
        case 2:
            record.grant.lifetime = AYNI_GRANT_CURRENT_BOOT;
            break;
        case 3:
            record.depth = AYNI_GRANT_DELEGATION_DEPTH_MAX_MAX + 1U;
            break;
        case 4:
            record.grant.parent_grant_id = 1;
            record.depth = 1;
            break;
        case 5:
            record.grant.parent_grant_id = grant_id + 1U;
            break;
        case 6:
            record.replaced_grant_id = 1;
            break;
        case 7:
            record.replaced_grant_id = record.grant_id;
            break;
        case 8:
            record.grant.source_domain.size = AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX + 1U;
            break;
        case 9:
            record.scope.size = AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX + 1U;
            break;
        case 10:
            record.grant.lifetime = 3;
            break;
        case 11:
            record.grant.subject_instance_id = 0;
            break;
        case 12:
            record.grant.owner_principal_id = 0;
            break;
        case 13:
            record.grant.capability_id = {};
            break;
        case 14:
            record.grant.capability_version = 0;
            break;
        default:
            record.grant_id = 1;
            break;
        }
        EXPECT_FALSE(valid_grant_record(record)) << fault;
    }
    GrantSnapshotRecord snapshot{};
    snapshot.revision = 91;
    EXPECT_FALSE(read_grant_snapshot_record(nullptr, 1000, snapshot));
    EXPECT_EQ(snapshot.revision, 91U);
}

TEST(GrantRecords, syntactically_valid_expanded_scope_outside_registered_domain_is_rejected) {
    // Independent format1 bytes: one count-valued I64 field with domain [0,10].
    constexpr std::array<uint8_t, 33> full{1, 0,   1,   0,   1,   0,   0, 0, 1, 1, 5,
                                           0, 'c', 'o', 'u', 'n', 't', 0, 0, 0, 0, 0,
                                           0, 0,   0,   10,  0,   0,   0, 0, 0, 0, 0};
    auto outside = full;
    outside[17] = 1;
    outside[25] = 11; // [1,11] is valid syntax, but not a subset of [0,10].
    const ayni_bytes_v1 registered{full.data(), static_cast<uint32_t>(full.size())};
    const ayni_bytes_v1 expanded{outside.data(), static_cast<uint32_t>(outside.size())};
    ayni::catalog::DomainReader syntax(expanded);
    ayni::catalog::DomainField field{};
    ASSERT_TRUE(syntax.next(field));
    EXPECT_FALSE(syntax.next(field));
    ASSERT_TRUE(syntax.ok());
    const auto check = covers(registered, registered, expanded);
    ASSERT_EQ(check.status, AYNI_INVALID_ARGUMENT);
    ASSERT_FALSE(check.covered);
    auto grant = root();
    grant.source_domain_size = registered.size;
    grant.scope_size = expanded.size;
    std::vector<uint8_t> bytes(grant_record_size(grant));
    write_grant_record(grant, registered, expanded, bytes.data());
    GrantRecord decoded{};
    decoded.grant_id = 91;
    EXPECT_FALSE(read_grant_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded));
    EXPECT_EQ(decoded.grant_id, 91U);
    ASSERT_TRUE(
        read_grant_record(root_body.data(), static_cast<uint32_t>(root_body.size()), decoded));
    decoded.grant.source_domain = registered;
    decoded.scope = expanded;
    EXPECT_FALSE(valid_grant_record(decoded));
    outside[25] = 10; // [1,10] is a valid nonempty narrowing.
    EXPECT_TRUE(valid_grant_record(decoded));
    write_grant_record(grant, registered, expanded, bytes.data());
    EXPECT_TRUE(read_grant_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded));
}

} // namespace
