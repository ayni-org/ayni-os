#include "catalog/capability/capability_registry.h"
#include "catalog/capability/field_domain.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

using ayni::catalog::CapabilityRegistry;
using ayni::catalog::DomainField;
using ayni::catalog::DomainReader;

ayni_string_v1 token(const char *text) {
    return {text, static_cast<uint32_t>(std::strlen(text))};
}

ayni_authority_value_set_v1 integer(int64_t minimum, int64_t maximum, const char *unit = "count") {
    ayni_authority_value_set_v1 value{};
    value.struct_size = sizeof(value);
    value.kind = AYNI_AUTHORITY_I64;
    value.unit = token(unit);
    value.values.integer = {minimum, maximum};
    return value;
}

ayni_authority_value_set_v1 tokens(ayni_authority_value_kind_t kind, const ayni_string_v1 *members,
                                   uint32_t count) {
    ayni_authority_value_set_v1 value{};
    value.struct_size = sizeof(value);
    value.kind = kind;
    value.values.tokens = {members, count, 0};
    return value;
}

ayni_authority_field_desc_v1 field(uint32_t id, const ayni_authority_value_set_v1 &domain,
                                   ayni_authority_field_usage_t usage = AYNI_AUTHORITY_PARAMETER) {
    return {sizeof(ayni_authority_field_desc_v1), id, usage, 0, domain};
}

ayni_capability_desc_v1 description(const ayni_authority_field_desc_v1 *fields = nullptr,
                                    uint32_t count = 0, uint32_t version = 1,
                                    const char *id = "test.capability") {
    return {sizeof(ayni_capability_desc_v1), version, token(id), count, 0, fields};
}

class Registry {
  public:
    Registry(uint32_t capacity = 4, uint32_t bytes = 256)
        : memory_(CapabilityRegistry::storage_size(capacity, bytes) / sizeof(uint64_t) + 1U),
          registry_(memory_.data(), capacity, bytes) {}
    CapabilityRegistry *operator->() { return &registry_; }

  private:
    std::vector<uint64_t> memory_;
    CapabilityRegistry registry_;
};

std::vector<uint8_t> copied(ayni_bytes_v1 bytes) {
    return {bytes.data, bytes.data + bytes.size};
}

TEST(CapabilityRegistry, exact_versions_coexist_and_unknown_id_and_version_are_distinct) {
    Registry registry(2);
    ASSERT_EQ(registry->add(description()), AYNI_OK);
    ASSERT_EQ(registry->add(description(nullptr, 0, 2)), AYNI_OK);
    EXPECT_EQ(registry->find(token("test.capability"), 1).kind,
              CapabilityRegistry::Lookup::Kind::found);
    EXPECT_EQ(registry->find(token("test.capability"), 2).index, 1U);
    EXPECT_EQ(registry->find(token("test.capability"), 3).kind,
              CapabilityRegistry::Lookup::Kind::unknown_version);
    EXPECT_EQ(registry->find(token("unknown"), 1).kind,
              CapabilityRegistry::Lookup::Kind::unknown_id);
    EXPECT_EQ(registry->add(description()), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(registry->add(description(nullptr, 0, 3)), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(registry->size(), 2U);
}

TEST(CapabilityRegistry, empty_field_domain_has_a_versioned_canonical_body) {
    Registry registry;
    ASSERT_EQ(registry->add(description()), AYNI_OK);
    EXPECT_EQ(copied(registry->domain_view(0)), (std::vector<uint8_t>{1, 0, 0, 0}));
}

TEST(CapabilityRegistry, fields_and_token_members_are_sorted_and_duplicate_members_are_removed) {
    const ayni_string_v1 members[] = {token("west"), token("east"), token("west")};
    const std::array<ayni_authority_field_desc_v1, 2> fields{
        field(9, integer(-5, 10)), field(2, tokens(AYNI_AUTHORITY_ENUM, members, 3))};
    Registry registry;
    ASSERT_EQ(registry->add(description(fields.data(), static_cast<uint32_t>(fields.size()))),
              AYNI_OK);
    DomainReader reader(registry->domain_view(0));
    DomainField decoded{};
    ASSERT_TRUE(reader.next(decoded));
    EXPECT_EQ(decoded.id, 2U);
    EXPECT_EQ(decoded.values.token_count, 2U);
    EXPECT_EQ(std::string(decoded.values.tokens[0].data, decoded.values.tokens[0].size), "east");
    EXPECT_EQ(std::string(decoded.values.tokens[1].data, decoded.values.tokens[1].size), "west");
    ASSERT_TRUE(reader.next(decoded));
    EXPECT_EQ(decoded.id, 9U);
    EXPECT_EQ(decoded.values.minimum, -5);
    EXPECT_EQ(decoded.values.maximum, 10);
    EXPECT_TRUE(reader.ok());
}

TEST(CapabilityRegistry, registration_copies_all_borrowed_identity_units_and_tokens) {
    char id[] = "camera.check";
    char unit[] = "count";
    char destination[] = "cloud";
    const ayni_string_v1 members[] = {token(destination)};
    std::array<ayni_authority_field_desc_v1, 2> fields{
        field(1, integer(1, 10, unit)),
        field(2, tokens(AYNI_AUTHORITY_IDENTIFIER, members, 1), AYNI_AUTHORITY_EGRESS_DESTINATION)};
    auto desc = description(fields.data(), static_cast<uint32_t>(fields.size()), 1, id);
    Registry registry;
    ASSERT_EQ(registry->add(desc), AYNI_OK);
    const auto before = copied(registry->domain_view(0));
    id[0] = 'x';
    unit[0] = 'x';
    destination[0] = 'x';
    fields[0].domain.values.integer = {99, 100};
    desc.version = 2;
    EXPECT_EQ(copied(registry->domain_view(0)), before);
    EXPECT_TRUE(registry->at(0).id.equals(token("camera.check")));
    EXPECT_EQ(registry->at(0).version, 1U);
}

TEST(CapabilityRegistry, invalid_top_level_descriptors_never_take_a_slot) {
    Registry registry;
    for (unsigned fault = 0; fault < 7; ++fault) {
        auto desc = description();
        switch (fault) {
        case 0:
            --desc.struct_size;
            break;
        case 1:
            desc.version = 0;
            break;
        case 2:
            desc.capability_id = token("Bad");
            break;
        case 3:
            desc.capability_id = {nullptr, 1};
            break;
        case 4:
            desc.reserved = 1;
            break;
        case 5:
            desc.field_count = 1;
            break;
        default:
            desc.field_count = 9;
            break;
        }
        EXPECT_EQ(registry->add(desc), AYNI_INVALID_ARGUMENT) << fault;
        EXPECT_EQ(registry->size(), 0U);
    }
    auto newer = description();
    ++newer.struct_size;
    EXPECT_EQ(registry->add(newer), AYNI_UNSUPPORTED_VERSION);
}

TEST(CapabilityRegistry, field_and_value_struct_sizes_are_checked_before_their_contents) {
    Registry registry;
    auto item = field(1, integer(0, 3));
    --item.struct_size;
    EXPECT_EQ(registry->add(description(&item, 1)), AYNI_INVALID_ARGUMENT);
    item.struct_size = sizeof(item) + 1U;
    EXPECT_EQ(registry->add(description(&item, 1)), AYNI_UNSUPPORTED_VERSION);
    item.struct_size = sizeof(item);
    --item.domain.struct_size;
    EXPECT_EQ(registry->add(description(&item, 1)), AYNI_INVALID_ARGUMENT);
    item.domain.struct_size = sizeof(item.domain) + 1U;
    EXPECT_EQ(registry->add(description(&item, 1)), AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(registry->size(), 0U);
}

TEST(CapabilityRegistry,
     pure_validation_reports_needed_bytes_without_taking_a_slot_or_publishing_failures) {
    auto item = field(1, integer(0, 3));
    const auto desc = description(&item, 1);
    ayni::catalog::DomainEncoding needed{};
    needed.size = 91;
    ASSERT_EQ(CapabilityRegistry::validate(desc, needed), AYNI_OK);
    EXPECT_EQ(needed.size, 33U);
    --item.domain.struct_size;
    needed.size = 91;
    EXPECT_EQ(CapabilityRegistry::validate(desc, needed), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(needed.size, 91U);
    item.domain.struct_size = sizeof(item.domain) + 1U;
    EXPECT_EQ(CapabilityRegistry::validate(desc, needed), AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(needed.size, 91U);
}

TEST(CapabilityRegistry, malformed_field_identity_usage_type_or_unit_is_rejected) {
    Registry registry;
    for (unsigned fault = 0; fault < 7; ++fault) {
        auto item = field(1, integer(0, 3));
        switch (fault) {
        case 0:
            item.field_id = 0;
            break;
        case 1:
            item.reserved = 1;
            break;
        case 2:
            item.usage = 0;
            break;
        case 3:
            item.usage = AYNI_AUTHORITY_EXECUTION_TARGET;
            break;
        case 4:
            item.domain.kind = 0;
            break;
        case 5:
            item.domain.unit = token("");
            break;
        default:
            item.domain.values.integer = {4, 3};
            break;
        }
        EXPECT_EQ(registry->add(description(&item, 1)), AYNI_INVALID_ARGUMENT) << fault;
    }
    auto first = field(1, integer(0, 3));
    const std::array<ayni_authority_field_desc_v1, 2> duplicate{first, first};
    EXPECT_EQ(registry->add(description(duplicate.data(), static_cast<uint32_t>(duplicate.size()))),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(registry->size(), 0U);
}

TEST(CapabilityRegistry, token_count_is_bounded_before_deduplication_or_member_access) {
    Registry registry;
    const ayni_string_v1 one[] = {token("east")};
    for (const uint32_t count : {0U, 9U}) {
        auto item = field(1, tokens(AYNI_AUTHORITY_ENUM, one, count));
        EXPECT_EQ(registry->add(description(&item, 1)), AYNI_INVALID_ARGUMENT);
    }
    auto item = field(1, tokens(AYNI_AUTHORITY_ENUM, nullptr, 1));
    EXPECT_EQ(registry->add(description(&item, 1)), AYNI_INVALID_ARGUMENT);
    item.domain = tokens(AYNI_AUTHORITY_ENUM, one, 1);
    item.domain.values.tokens.reserved = 1;
    EXPECT_EQ(registry->add(description(&item, 1)), AYNI_INVALID_ARGUMENT);
    item.domain.values.tokens.reserved = 0;
    item.domain.unit = token("count");
    EXPECT_EQ(registry->add(description(&item, 1)), AYNI_INVALID_ARGUMENT);
}

TEST(CapabilityRegistry, token_length_and_alphabet_match_catalog_ids) {
    Registry registry;
    for (const char *text : {"", "UPPER", "with space", "abcdefghijklmnopqrstuvwxyz0123456"}) {
        const ayni_string_v1 members[] = {token(text)};
        const auto item = field(1, tokens(AYNI_AUTHORITY_ENUM, members, 1));
        EXPECT_EQ(registry->add(description(&item, 1)), AYNI_INVALID_ARGUMENT) << text;
    }
    const ayni_string_v1 members[] = {token("abcdefghijklmnopqrstuvwxyz012345")};
    const auto item = field(1, tokens(AYNI_AUTHORITY_ENUM, members, 1));
    EXPECT_EQ(registry->add(description(&item, 1)), AYNI_OK);
}

TEST(CapabilityRegistry, execution_target_and_egress_are_distinct_single_identifier_fields) {
    Registry registry;
    const ayni_string_v1 members[] = {token("local"), token("cloud")};
    auto target =
        field(1, tokens(AYNI_AUTHORITY_IDENTIFIER, members, 2), AYNI_AUTHORITY_EXECUTION_TARGET);
    auto egress =
        field(2, tokens(AYNI_AUTHORITY_IDENTIFIER, members, 2), AYNI_AUTHORITY_EGRESS_DESTINATION);
    std::array<ayni_authority_field_desc_v1, 2> fields{target, egress};
    EXPECT_EQ(registry->add(description(fields.data(), static_cast<uint32_t>(fields.size()))),
              AYNI_OK);
    fields[1].usage = AYNI_AUTHORITY_EXECUTION_TARGET;
    EXPECT_EQ(registry->add(description(fields.data(), static_cast<uint32_t>(fields.size()), 2)),
              AYNI_INVALID_ARGUMENT);
    fields[0].usage = AYNI_AUTHORITY_EGRESS_DESTINATION;
    fields[1].usage = AYNI_AUTHORITY_EGRESS_DESTINATION;
    EXPECT_EQ(registry->add(description(fields.data(), static_cast<uint32_t>(fields.size()), 2)),
              AYNI_INVALID_ARGUMENT);
    fields[0] = target;
    fields[1] = egress;
    fields[0].domain.kind = AYNI_AUTHORITY_ENUM;
    EXPECT_EQ(registry->add(description(fields.data(), static_cast<uint32_t>(fields.size()), 2)),
              AYNI_INVALID_ARGUMENT);
}

TEST(CapabilityRegistry,
     complete_domain_fits_exactly_or_rejects_one_byte_more_without_partial_registration) {
    std::array<ayni_string_v1, 8> members{token("aaaa"),    token("bbbb"),    token("cccc"),
                                          token("dddd"),    token("eeeeeee"), token("fffffff"),
                                          token("ggggggg"), token("hhhhhhh")};
    auto item = field(1, tokens(AYNI_AUTHORITY_IDENTIFIER, members.data(),
                                static_cast<uint32_t>(members.size())));
    Registry registry(2, 64);
    ASSERT_EQ(registry->add(description(&item, 1)), AYNI_OK);
    EXPECT_EQ(registry->domain_view(0).size, 64U);
    const auto before = copied(registry->domain_view(0));
    members[7] = token("hhhhhhhh");
    EXPECT_EQ(registry->add(description(&item, 1, 2)), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(registry->size(), 1U);
    EXPECT_EQ(copied(registry->domain_view(0)), before);
}

TEST(CapabilityRegistry,
     largest_field_and_member_counts_fit_the_explicit_maximum_byte_configuration) {
    std::array<std::string, 8> words{};
    std::array<ayni_string_v1, 8> members{};
    std::array<ayni_authority_field_desc_v1, 8> fields{};
    for (uint32_t index = 0; index < 8; ++index) {
        words[index] = std::string(32, static_cast<char>('a' + index));
        members[index] = token(words[index].c_str());
    }
    for (uint32_t index = 0; index < 8; ++index) {
        fields[index] = field(index + 1U, tokens(AYNI_AUTHORITY_IDENTIFIER, members.data(),
                                                 static_cast<uint32_t>(members.size())));
    }
    Registry registry(1, AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX);
    ASSERT_EQ(registry->add(description(fields.data(), static_cast<uint32_t>(fields.size()))),
              AYNI_OK);
    EXPECT_EQ(registry->domain_view(0).size, 2180U);
    EXPECT_EQ(CapabilityRegistry::storage_size(256, 4096),
              256U * sizeof(ayni::catalog::Capability) + 256U * 4096U);
    EXPECT_LE(CapabilityRegistry::storage_size(256, 4096), std::numeric_limits<uint32_t>::max());
}

TEST(CapabilityRegistry,
     signed_domain_extremes_round_trip_without_unsigned_conversion_or_subtraction) {
    const auto item = field(1, integer(std::numeric_limits<int64_t>::min(),
                                       std::numeric_limits<int64_t>::max(), "scalar"));
    Registry registry;
    ASSERT_EQ(registry->add(description(&item, 1)), AYNI_OK);
    DomainReader reader(registry->domain_view(0));
    DomainField decoded{};
    ASSERT_TRUE(reader.next(decoded));
    EXPECT_EQ(decoded.values.minimum, std::numeric_limits<int64_t>::min());
    EXPECT_EQ(decoded.values.maximum, std::numeric_limits<int64_t>::max());
    EXPECT_TRUE(reader.ok());
}

TEST(CapabilityRegistry, filling_the_registry_preserves_every_prior_domain_and_lookup) {
    for (const uint32_t capacity : {1U, 2U, 4U}) {
        SCOPED_TRACE(capacity);
        Registry registry(capacity, 64);
        std::vector<std::vector<uint8_t>> accepted;
        for (uint32_t version = 1; version <= capacity; ++version) {
            const auto item = field(1, integer(10 * version, 10 * version + 9));
            std::array<uint8_t, 64> expected{};
            uint32_t size = 0;
            ASSERT_EQ(ayni::catalog::encode_domain(&item, 1, expected.data(),
                                                   static_cast<uint32_t>(expected.size()), size),
                      AYNI_OK);
            accepted.emplace_back(expected.begin(), expected.begin() + size);
            ASSERT_EQ(registry->add(description(&item, 1, version)), AYNI_OK);
            for (uint32_t previous = 1; previous <= version; ++previous) {
                const auto found = registry->find(token("test.capability"), previous);
                ASSERT_EQ(found.kind, CapabilityRegistry::Lookup::Kind::found);
                EXPECT_EQ(copied(registry->domain_view(found.index)), accepted[previous - 1U]);
                DomainReader reader(registry->domain_view(found.index));
                DomainField decoded{};
                ASSERT_TRUE(reader.next(decoded));
                EXPECT_EQ(decoded.values.minimum, 10 * previous);
                EXPECT_EQ(decoded.values.maximum, 10 * previous + 9);
                EXPECT_TRUE(reader.ok());
            }
        }
    }
}

// C-AUTH-017: a duplicate consumes no set slot, but later members still matter.
TEST(CapabilityRegistry, a_repeated_member_does_not_hide_later_distinct_members) {
    const ayni_string_v1 members[] = {token("west"), token("west"), token("east")};
    const auto item = field(1, tokens(AYNI_AUTHORITY_ENUM, members, 3));
    Registry registry;
    ASSERT_EQ(registry->add(description(&item, 1)), AYNI_OK);
    const std::vector<uint8_t> expected{1, 0, 1,   0,   1,   0,   0, 0,   2,   1,   0,
                                        2, 4, 'e', 'a', 's', 't', 4, 'w', 'e', 's', 't'};
    EXPECT_EQ(copied(registry->domain_view(0)), expected);
}

// ADR-0034 decision 3: the standalone encoder accepts an exact-size buffer;
// only a capacity failure publishes the required size before writing bytes.
TEST(CapabilityRegistry, standalone_domain_encoder_probes_and_fills_exact_capacity) {
    const auto item = field(1, integer(1, 2));
    std::array<uint8_t, 34> output{};
    output.fill(0xa5);
    const auto sentinel = output;
    uint32_t size = 91;
    EXPECT_EQ(ayni::catalog::encode_domain(&item, 1, nullptr, 0, size), AYNI_BUFFER_TOO_SMALL);
    ASSERT_EQ(size, 33U);
    EXPECT_EQ(ayni::catalog::encode_domain(&item, 1, output.data(), 32, size),
              AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(size, 33U);
    EXPECT_EQ(output, sentinel);
    ASSERT_EQ(ayni::catalog::encode_domain(&item, 1, output.data(), 33, size), AYNI_OK);
    const std::vector<uint8_t> expected{1, 0, 1, 0, 1, 0, 0, 0, 1, 1, 5, 0, 'c', 'o', 'u', 'n', 't',
                                        1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0,   0,   0,   0};
    EXPECT_EQ((std::vector<uint8_t>{output.begin(), output.begin() + size}), expected);
    EXPECT_EQ(size, 33U);
    EXPECT_EQ(output.back(), 0xa5);

    ASSERT_EQ(ayni::catalog::encode_domain(nullptr, 0, output.data(), 4, size), AYNI_OK);
    EXPECT_EQ(size, 4U);
    EXPECT_EQ((std::vector<uint8_t>{output.begin(), output.begin() + size}),
              (std::vector<uint8_t>{1, 0, 0, 0}));
}

// ADR-0034 decisions 3/6: even an invalid later field must preserve both
// caller outputs; a newer ABI structure retains its classified error.
TEST(CapabilityRegistry, standalone_domain_encoder_preserves_outputs_for_invalid_later_fields) {
    std::array<ayni_authority_field_desc_v1, 2> fields{field(1, integer(1, 2)),
                                                       field(2, integer(3, 4))};
    std::array<uint8_t, 128> output{};
    output.fill(0xa5);
    const auto sentinel = output;
    uint32_t size = 91;
    fields[1].domain.values.integer = {4, 3};
    EXPECT_EQ(ayni::catalog::encode_domain(fields.data(), 2, output.data(), output.size(), size),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(output, sentinel);
    EXPECT_EQ(size, 91U);
    ++fields[1].struct_size;
    EXPECT_EQ(ayni::catalog::encode_domain(fields.data(), 2, output.data(), output.size(), size),
              AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(output, sentinel);
    EXPECT_EQ(size, 91U);
}

} // namespace
