// ADR-0030/C-WIRE-017: independent identity, before catalog/subject checks.
#include "authority/scope/scope.h"
#include "catalog/capability/field_domain.h"
#include "foundation/little_endian.h"
#include "kernel/command/command_canonical.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace {
using namespace ayni::kernel;

ayni_string_v1 text(const char *value) {
    return {value, static_cast<uint32_t>(std::strlen(value))};
}

ayni_grant_create_v1 request() {
    ayni_grant_create_v1 value{};
    value.struct_size = sizeof(value);
    value.capability_version = 1;
    value.capability_id = text("a");
    value.subject_instance_id = 1;
    value.lifetime = AYNI_GRANT_PERMANENT;
    value.scope.struct_size = sizeof(value.scope);
    return value;
}

ayni_authority_value_set_v1 number(int64_t minimum, int64_t maximum, const char *unit = "ms") {
    ayni_authority_value_set_v1 value{};
    value.struct_size = sizeof(value);
    value.kind = AYNI_AUTHORITY_I64;
    value.unit = text(unit);
    value.values.integer = {minimum, maximum};
    return value;
}

ayni_authority_value_set_v1 choices(const ayni_string_v1 *members, uint32_t count) {
    ayni_authority_value_set_v1 value{};
    value.struct_size = sizeof(value);
    value.kind = AYNI_AUTHORITY_IDENTIFIER;
    value.values.tokens = {members, count, 0};
    return value;
}

std::vector<uint8_t> encoded(const ayni_grant_create_v1 &grant, uint32_t expected = 0) {
    uint32_t size = 0;
    EXPECT_EQ(measure_authority_command({expected, &grant}, size), AYNI_OK);
    std::vector<uint8_t> bytes(size);
    EXPECT_EQ(encode_authority_command({expected, &grant}, bytes.data(), size, size), AYNI_OK);
    return bytes;
}
ayni_bytes_v1 view(const std::vector<uint8_t> &bytes) {
    return {bytes.data(), static_cast<uint32_t>(bytes.size())};
}

TEST(CommandCanonical, empty_raw_scope_has_an_independent_fixed_byte_oracle) {
    const auto bytes = encoded(request());
    // Contract order, independently spelled out; no production size/offsets.
    const std::vector<uint8_t> expected{9, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 'a', 1, 0,
                                        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0,   0, 0,
                                        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 1, 0,   0, 0};
    EXPECT_EQ(bytes, expected);
    AuthorityCommandRecord record{};
    ASSERT_TRUE(read_authority_command(view(bytes), record));
    EXPECT_EQ(record.subject_instance_id, 1U);
    EXPECT_EQ(record.raw_scope.size, 4U);
    EXPECT_EQ(authority_command_max_bytes(256), 340U);
}

TEST(CommandCanonical, sorting_and_deduplication_keep_omission_distinct_from_explicit_constraints) {
    auto grant = request();
    const ayni_string_v1 unordered[] = {text("west"), text("east"), text("west")};
    const ayni_string_v1 ordered[] = {text("east"), text("west")};
    const ayni_scope_field_v1 first[] = {{sizeof(ayni_scope_field_v1), 9, number(-1, 2)},
                                         {sizeof(ayni_scope_field_v1), 2, choices(unordered, 3)}};
    const ayni_scope_field_v1 second[] = {{sizeof(ayni_scope_field_v1), 2, choices(ordered, 2)},
                                          {sizeof(ayni_scope_field_v1), 9, number(-1, 2)}};
    const auto omitted = encoded(grant);
    grant.scope = {sizeof(grant.scope), 2, first};
    const auto bytes = encoded(grant);
    grant.scope.fields = second;
    EXPECT_EQ(encoded(grant), bytes);
    EXPECT_NE(bytes, omitted);
    AuthorityCommandRecord record{};
    ASSERT_TRUE(read_authority_command(view(bytes), record));
    const std::vector<uint8_t> expected_scope{
        1,   0,   2,   0,   2,   0,   0,   0,   3, 0, 2, 0, 4, 'e', 'a', 's',
        't', 4,   'w', 'e', 's', 't', 9,   0,   0, 0, 1, 2, 0, 0,   'm', 's',
        255, 255, 255, 255, 255, 255, 255, 255, 2, 0, 0, 0, 0, 0,   0,   0};
    EXPECT_EQ((std::vector<uint8_t>{record.raw_scope.data,
                                    record.raw_scope.data + record.raw_scope.size}),
              expected_scope);
}

TEST(CommandCanonical, identity_needs_no_catalog_subject_or_parent_and_includes_request_scalars) {
    auto grant = request();
    grant.capability_id = text("not.registered");
    grant.capability_version = 999;
    grant.subject_instance_id = 12345;
    grant.parent_grant_id = 98765;
    grant.lifetime = AYNI_GRANT_CURRENT_BOOT;
    grant.duration_us = 100;
    const auto original = encoded(grant, 7);
    EXPECT_NE(encoded(grant, 8), original);
    ++grant.duration_us;
    EXPECT_NE(encoded(grant, 7), original);
    grant.flags = AYNI_GRANT_REQUIRES_SCENE | AYNI_GRANT_REQUIRES_QUOTA;
    const auto unsupported = encoded(grant, 7);
    AuthorityCommandRecord record{};
    EXPECT_TRUE(read_authority_command(view(unsupported), record));
    // Known flags have shape; the Kernel rejects their unsupported semantics
    // only after it has resolved an earlier mapped command's identity.
    EXPECT_EQ(record.flags, grant.flags);
}

TEST(CommandCanonical, malformed_nested_inputs_and_short_buffers_publish_no_partial_output) {
    auto grant = request();
    std::array<ayni_scope_field_v1, 2> fields{{{sizeof(ayni_scope_field_v1), 1, number(0, 10)},
                                               {sizeof(ayni_scope_field_v1), 2, number(4, 3)}}};
    grant.scope = {sizeof(grant.scope), 2, fields.data()};
    std::array<uint8_t, 256> bytes{};
    bytes.fill(0xA5);
    const auto before = bytes;
    uint32_t size = 91;
    EXPECT_EQ(encode_authority_command({0, &grant}, bytes.data(),
                                       static_cast<uint32_t>(bytes.size()), size),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(size, 91U);
    EXPECT_EQ(bytes, before);
    fields[1].allowed = number(3, 4);
    ++fields[1].allowed.struct_size;
    EXPECT_EQ(measure_authority_command({0, &grant}, size), AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(size, 91U);
    --fields[1].allowed.struct_size;
    uint32_t needed = 0;
    ASSERT_EQ(measure_authority_command({0, &grant}, needed), AYNI_OK);
    EXPECT_EQ(encode_authority_command({0, &grant}, bytes.data(), needed - 1, size),
              AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(size, needed);
    EXPECT_EQ(bytes, before);
}

TEST(CommandCanonical,
     malformed_counts_scalars_units_and_duplicate_fields_are_rejected_before_access) {
    auto grant = request();
    uint32_t size = 13;
    grant.scope.field_count = 9;
    EXPECT_EQ(measure_authority_command({0, &grant}, size), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(size, 13U);
    grant = request();
    const ayni_string_v1 one[] = {text("east")};
    auto field = ayni_scope_field_v1{sizeof(ayni_scope_field_v1), 1, choices(one, 9)};
    grant.scope = {sizeof(grant.scope), 1, &field};
    EXPECT_EQ(measure_authority_command({0, &grant}, size), AYNI_INVALID_ARGUMENT);
    field.allowed = number(0, 1, "");
    EXPECT_EQ(measure_authority_command({0, &grant}, size), AYNI_INVALID_ARGUMENT);
    field.allowed = number(0, 1);
    const ayni_scope_field_v1 duplicate[] = {field, field};
    grant.scope = {sizeof(grant.scope), 2, duplicate};
    EXPECT_EQ(measure_authority_command({0, &grant}, size), AYNI_INVALID_ARGUMENT);
    grant = request();
    EXPECT_EQ(measure_authority_command({1, &grant}, size), AYNI_INVALID_ARGUMENT);
    grant.parent_grant_id = 1;
    EXPECT_EQ(measure_authority_command({0, &grant}, size), AYNI_INVALID_ARGUMENT);
    grant.flags = 0x80000000U;
    EXPECT_EQ(measure_authority_command({1, &grant}, size), AYNI_INVALID_ARGUMENT);
}

TEST(CommandCanonical,
     strict_reader_rejects_every_truncation_trailing_and_noncanonical_field_order) {
    auto grant = request();
    const ayni_scope_field_v1 fields[] = {{sizeof(ayni_scope_field_v1), 1, number(-1, 2)},
                                          {sizeof(ayni_scope_field_v1), 2, number(-1, 2)}};
    grant.scope = {sizeof(grant.scope), 2, fields};
    const auto bytes = encoded(grant);
    AuthorityCommandRecord out{};
    out.subject_instance_id = 54321;
    for (uint32_t size = 0; size < bytes.size(); ++size) {
        EXPECT_FALSE(read_authority_command({bytes.data(), size}, out)) << size;
        EXPECT_EQ(out.subject_instance_id, 54321U);
    }
    auto damaged = bytes;
    damaged.push_back(0);
    EXPECT_FALSE(read_authority_command(view(damaged), out));
    AuthorityCommandRecord decoded{};
    ASSERT_TRUE(read_authority_command(view(bytes), decoded));
    const auto scope_at = static_cast<std::size_t>(decoded.raw_scope.data - bytes.data());
    for (const std::size_t at : {scope_at, scope_at + 3, scope_at + 4 + 7}) {
        damaged = bytes;
        damaged[at] = 2; // unknown format/reserved bits
        EXPECT_FALSE(read_authority_command(view(damaged), out)) << at;
    }
    damaged = bytes;
    // First numeric field occupies 8-byte header, 2-byte unit, 16-byte value.
    damaged[scope_at + 4 + 26] = 1; // duplicate second ID
    EXPECT_FALSE(read_authority_command(view(damaged), out));
}

TEST(CommandCanonical, strict_reader_rejects_duplicate_or_unsorted_tokens_and_integer_reversal) {
    auto grant = request();
    const ayni_string_v1 two[] = {text("a"), text("b")};
    ayni_scope_field_v1 field{sizeof(field), 1, choices(two, 2)};
    grant.scope = {sizeof(grant.scope), 1, &field};
    auto bytes = encoded(grant);
    AuthorityCommandRecord decoded{};
    ASSERT_TRUE(read_authority_command(view(bytes), decoded));
    auto at = static_cast<std::size_t>(decoded.raw_scope.data - bytes.data());
    bytes[at + 4 + 8 + 3] = 'a'; // equal second token
    EXPECT_FALSE(read_authority_command(view(bytes), decoded));
    field.allowed = number(1, 2);
    bytes = encoded(grant);
    ASSERT_TRUE(read_authority_command(view(bytes), decoded));
    at = static_cast<std::size_t>(decoded.raw_scope.data - bytes.data());
    bytes[at + 4 + 8 + 2] = 3; // min=3, max=2
    EXPECT_FALSE(read_authority_command(view(bytes), decoded));
}

TEST(CommandCanonical, signed_extremes_and_maximum_shape_fit_the_declared_hard_bounds) {
    auto grant = request();
    ayni_scope_field_v1 integer{
        sizeof(integer), 1,
        number(std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max())};
    grant.scope = {sizeof(grant.scope), 1, &integer};
    auto bytes = encoded(grant);
    AuthorityCommandRecord decoded{};
    EXPECT_TRUE(read_authority_command(view(bytes), decoded));
    const char *long_tokens[] = {
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        "cccccccccccccccccccccccccccccccc", "dddddddddddddddddddddddddddddddd",
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", "ffffffffffffffffffffffffffffffff",
        "gggggggggggggggggggggggggggggggg", "hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh"};
    std::array<ayni_string_v1, 8> members{};
    std::array<ayni_scope_field_v1, 8> fields{};
    for (uint32_t i = 0; i < 8; ++i) {
        members[i] = text(long_tokens[i]);
    }
    for (uint32_t i = 0; i < 8; ++i) {
        fields[i] = {sizeof(ayni_scope_field_v1), i + 1, choices(members.data(), 8)};
    }
    grant.capability_id = text("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    grant.scope = {sizeof(grant.scope), 8, fields.data()};
    bytes = encoded(grant);
    EXPECT_EQ(bytes.size(), 2264U); // 84-byte prefix bound + 2180-byte raw scope
    EXPECT_TRUE(read_authority_command(view(bytes), decoded));
    EXPECT_LE(bytes.size(), authority_command_max_bytes(4096));
}

TEST(CommandCanonical, pairing_proves_the_exact_expansion_without_a_second_scope_buffer) {
    const ayni_string_v1 endpoints[] = {text("east"), text("west")};
    const ayni_authority_field_desc_v1 fields[] = {
        {sizeof(ayni_authority_field_desc_v1), 1, AYNI_AUTHORITY_PARAMETER, 0, number(0, 10)},
        {sizeof(ayni_authority_field_desc_v1), 2, AYNI_AUTHORITY_EGRESS_DESTINATION, 0,
         choices(endpoints, 2)}};
    std::array<uint8_t, 256> domain{};
    uint32_t domain_size = 0;
    ASSERT_EQ(ayni::catalog::encode_domain(fields, 2, domain.data(),
                                           static_cast<uint32_t>(domain.size()), domain_size),
              AYNI_OK);
    const ayni_bytes_v1 source{domain.data(), domain_size};
    auto grant = request();
    ayni_scope_field_v1 narrowed{sizeof(narrowed), 1, number(2, 4)};
    grant.scope = {sizeof(grant.scope), 1, &narrowed};
    const auto bytes = encoded(grant);
    AuthorityCommandRecord command{};
    ASSERT_TRUE(read_authority_command(view(bytes), command));
    std::array<uint8_t, 256> scope{};
    uint32_t size = 0;
    ASSERT_EQ(ayni::authority::normalize_scope(source, grant.scope, scope.data(),
                                               static_cast<uint32_t>(scope.size()), size),
              AYNI_OK);
    EXPECT_TRUE(authority_command_scope_matches(command, source, {scope.data(), size}));
    narrowed.allowed = number(2, 5);
    ASSERT_EQ(ayni::authority::normalize_scope(source, grant.scope, scope.data(),
                                               static_cast<uint32_t>(scope.size()), size),
              AYNI_OK);
    // Both scopes remain legal subsets; an unrelated wider result is still
    // not the request's exact expansion.
    EXPECT_FALSE(authority_command_scope_matches(command, source, {scope.data(), size}));
    narrowed.field_id = 9;
    const auto unknown_bytes = encoded(grant);
    ASSERT_TRUE(read_authority_command(view(unknown_bytes), command));
    EXPECT_FALSE(authority_command_scope_matches(command, source, {scope.data(), size}));
}

TEST(CommandCanonical, pairing_expands_omission_against_source_and_rejects_raw_or_stored_damage) {
    auto field = ayni_authority_field_desc_v1{sizeof(ayni_authority_field_desc_v1), 1,
                                              AYNI_AUTHORITY_PARAMETER, 0, number(0, 10)};
    std::array<uint8_t, 128> domain{};
    uint32_t size = 0;
    ASSERT_EQ(ayni::catalog::encode_domain(&field, 1, domain.data(),
                                           static_cast<uint32_t>(domain.size()), size),
              AYNI_OK);
    const auto original = domain;
    const auto bytes = encoded(request());
    AuthorityCommandRecord command{};
    ASSERT_TRUE(read_authority_command(view(bytes), command));
    EXPECT_TRUE(
        authority_command_scope_matches(command, {domain.data(), size}, {original.data(), size}));
    field.domain = number(0, 20);
    ASSERT_EQ(ayni::catalog::encode_domain(&field, 1, domain.data(),
                                           static_cast<uint32_t>(domain.size()), size),
              AYNI_OK);
    EXPECT_FALSE(
        authority_command_scope_matches(command, {domain.data(), size}, {original.data(), size}));
    auto damaged = bytes;
    damaged.back() = 1; // raw scope reserved bit
    command.raw_scope = {damaged.data() + damaged.size() - 4, 4};
    EXPECT_FALSE(
        authority_command_scope_matches(command, {domain.data(), size}, {domain.data(), size}));
    command.raw_scope = {bytes.data() + bytes.size() - 4, 4};
    EXPECT_FALSE(
        authority_command_scope_matches(command, {domain.data(), size}, {domain.data(), size - 1}));
}

TEST(CommandCanonical, invalid_identity_or_structure_never_publishes_a_size_or_bytes) {
    std::array<uint8_t, 256> output{};
    output.fill(0xA5);
    const auto before = output;
    const auto refuses = [&](const CanonicalCommandView &input, ayni_status_t status) {
        uint32_t size = 123;
        EXPECT_EQ(measure_authority_command(input, size), status);
        EXPECT_EQ(size, 123U);
        EXPECT_EQ(encode_authority_command(input, output.data(),
                                           static_cast<uint32_t>(output.size()), size),
                  status);
        EXPECT_EQ(size, 123U);
        EXPECT_EQ(output, before);
    };
    refuses({0, nullptr}, AYNI_INVALID_ARGUMENT);
    for (const auto status : {AYNI_INVALID_ARGUMENT, AYNI_UNSUPPORTED_VERSION}) {
        auto grant = request();
        grant.struct_size =
            status == AYNI_INVALID_ARGUMENT ? sizeof(grant) - 1U : sizeof(grant) + 1U;
        refuses({0, &grant}, status);
        grant = request();
        grant.scope.struct_size =
            status == AYNI_INVALID_ARGUMENT ? sizeof(grant.scope) - 1U : sizeof(grant.scope) + 1U;
        refuses({0, &grant}, status);
        grant = request();
        ayni_scope_field_v1 field{sizeof(field), 1, number(0, 1)};
        field.struct_size =
            status == AYNI_INVALID_ARGUMENT ? sizeof(field) - 1U : sizeof(field) + 1U;
        grant.scope = {sizeof(grant.scope), 1, &field};
        refuses({0, &grant}, status);
    }
    auto grant = request();
    grant.capability_version = 0;
    refuses({0, &grant}, AYNI_INVALID_ARGUMENT);
    grant = request();
    grant.capability_id = text("bad id");
    refuses({0, &grant}, AYNI_INVALID_ARGUMENT);
    grant = request();
    grant.subject_instance_id = 0;
    refuses({0, &grant}, AYNI_INVALID_ARGUMENT);
    grant = request();
    grant.duration_us = 1; // A permanent identity cannot carry a duration.
    refuses({0, &grant}, AYNI_INVALID_ARGUMENT);
    grant.lifetime = AYNI_GRANT_CURRENT_BOOT;
    grant.duration_us = 0;
    refuses({0, &grant}, AYNI_INVALID_ARGUMENT);
    grant.lifetime = 0;
    refuses({0, &grant}, AYNI_INVALID_ARGUMENT);
    grant = request();
    ayni_scope_field_v1 zero_id{sizeof(zero_id), 0, number(0, 1)};
    grant.scope = {sizeof(grant.scope), 1, &zero_id};
    refuses({0, &grant}, AYNI_INVALID_ARGUMENT);
}

TEST(CommandCanonical, null_output_has_distinct_measure_and_invalid_pointer_semantics) {
    const auto grant = request();
    uint32_t size = 123;
    EXPECT_EQ(encode_authority_command({0, &grant}, nullptr, 1, size), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(size, 123U);
    EXPECT_EQ(encode_authority_command({0, &grant}, nullptr, 0, size), AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(size, 57U);
    AuthorityCommandRecord output{};
    output.subject_instance_id = 123;
    EXPECT_FALSE(read_authority_command({nullptr, 0}, output));
    const std::vector<uint8_t> overlong(84U + 4096U + 1U, 0);
    EXPECT_FALSE(read_authority_command(view(overlong), output));
    EXPECT_EQ(output.subject_instance_id, 123U);
}

// A complete outer identity isolates raw-scope validation from truncated
// outer fields. Its length prefix changes with the damaged scope, so a
// rejection proves the raw scope itself is checked, not just BodyReader.
std::vector<uint8_t> with_raw_scope(const std::vector<uint8_t> &raw) {
    auto bytes = encoded(request());
    bytes.resize(bytes.size() - 4U); // The empty format-1 scope is four bytes.
    ayni::foundation::store_le32(bytes.data() + bytes.size() - 4U,
                                 static_cast<uint32_t>(raw.size()));
    bytes.insert(bytes.end(), raw.begin(), raw.end());
    return bytes;
}

bool raw_scope_of(const ayni_grant_create_v1 &grant, std::vector<uint8_t> &out) {
    const auto bytes = encoded(grant);
    AuthorityCommandRecord command{};
    if (!read_authority_command(view(bytes), command)) {
        return false;
    }
    out.assign(command.raw_scope.data, command.raw_scope.data + command.raw_scope.size);
    return true;
}

void expect_raw_scope_refused(const std::vector<uint8_t> &raw) {
    const auto bytes = with_raw_scope(raw);
    AuthorityCommandRecord output{};
    output.subject_instance_id = 123;
    output.duration_us = 456;
    EXPECT_FALSE(read_authority_command(view(bytes), output));
    EXPECT_EQ(output.subject_instance_id, 123U);
    EXPECT_EQ(output.duration_us, 456U);
}

TEST(CommandCanonical, complete_outer_length_cannot_hide_tail_bytes_inside_the_raw_scope) {
    auto grant = request();
    ayni_scope_field_v1 field{sizeof(field), 1, number(-1, 2)};
    for (const bool constrained : {false, true}) {
        SCOPED_TRACE(constrained);
        grant.scope = {sizeof(grant.scope), constrained ? 1U : 0U, constrained ? &field : nullptr};
        std::vector<uint8_t> raw;
        ASSERT_TRUE(raw_scope_of(grant, raw));
        const auto original = with_raw_scope(raw);
        AuthorityCommandRecord restored{};
        ASSERT_TRUE(read_authority_command(view(original), restored));
        raw.push_back(0xA5);
        // with_raw_scope updates the outer length prefix as well. The outer
        // BodyReader therefore consumes the entire identity successfully;
        // only the raw-scope reader can reject this internal trailing byte.
        expect_raw_scope_refused(raw);
    }
}

TEST(CommandCanonical, complete_outer_identity_still_rejects_malformed_raw_headers_and_integers) {
    // Raw format: four-byte header; then id/kind/unit-count/member-count/
    // reserved (eight bytes), unit bytes, and two little-endian i64 endpoints.
    constexpr uint32_t count_at = 2;
    constexpr uint32_t field_at = 4;
    constexpr uint32_t kind_at = field_at + 4;
    constexpr uint32_t unit_count_at = field_at + 5;
    constexpr uint32_t member_count_at = field_at + 6;
    constexpr uint32_t reserved_at = field_at + 7;
    constexpr uint32_t unit_at = field_at + 8;
    auto grant = request();
    ayni_scope_field_v1 field{sizeof(field), 1, number(-1, 2)};
    grant.scope = {sizeof(grant.scope), 1, &field};
    std::vector<uint8_t> raw;
    ASSERT_TRUE(raw_scope_of(grant, raw));
    for (const auto &change : std::array<std::pair<uint32_t, uint8_t>, 8>{{{0, 2},
                                                                           {3, 1},
                                                                           {count_at, 9},
                                                                           {field_at, 0},
                                                                           {kind_at, 0},
                                                                           {unit_count_at, 33},
                                                                           {member_count_at, 1},
                                                                           {reserved_at, 1}}}) {
        SCOPED_TRACE(change.first);
        auto damaged = raw;
        damaged[change.first] = change.second;
        expect_raw_scope_refused(damaged);
    }
    auto damaged = raw;
    damaged[unit_at] = ' ';
    expect_raw_scope_refused(damaged);
    damaged = raw;
    damaged[unit_count_at] = 3;
    damaged.resize(unit_at + 2U); // Declared unit does not fit the remaining bytes.
    expect_raw_scope_refused(damaged);
    damaged = raw;
    damaged.pop_back(); // Both endpoints must be complete.
    expect_raw_scope_refused(damaged);
    damaged = raw;
    damaged[count_at] = 2; // Declared second field has no complete header.
    expect_raw_scope_refused(damaged);
    expect_raw_scope_refused({1, 0, 0});
    expect_raw_scope_refused({1, 0, 1, 0});
}

TEST(CommandCanonical, enum_and_identifier_require_bounded_nonempty_sorted_tokens_without_units) {
    constexpr uint32_t kind_at = 4U + 4U;
    constexpr uint32_t unit_count_at = 4U + 5U;
    constexpr uint32_t member_count_at = 4U + 6U;
    constexpr uint32_t token_at = 4U + 8U;
    const ayni_string_v1 members[] = {text("a"), text("b")};
    auto grant = request();
    ayni_scope_field_v1 field{sizeof(field), 1, choices(members, 2)};
    field.allowed.kind = AYNI_AUTHORITY_ENUM;
    grant.scope = {sizeof(grant.scope), 1, &field};
    std::vector<uint8_t> raw;
    ASSERT_TRUE(raw_scope_of(grant, raw));
    const auto complete = with_raw_scope(raw);
    AuthorityCommandRecord decoded{};
    ASSERT_TRUE(read_authority_command(view(complete), decoded));
    for (const auto &change : std::array<std::pair<uint32_t, uint8_t>, 7>{{{unit_count_at, 1},
                                                                           {member_count_at, 0},
                                                                           {member_count_at, 9},
                                                                           {token_at, 0},
                                                                           {token_at, 33},
                                                                           {token_at + 1U, ' '},
                                                                           {token_at + 3U, 'a'}}}) {
        SCOPED_TRACE(change.first);
        auto damaged = raw;
        damaged[change.first] = change.second;
        expect_raw_scope_refused(damaged);
    }
    auto damaged = raw;
    damaged[kind_at] = AYNI_AUTHORITY_IDENTIFIER;
    damaged[token_at + 1U] = 'b';
    damaged[token_at + 3U] = 'a'; // Distinct but descending tokens are also invalid.
    expect_raw_scope_refused(damaged);
    damaged = raw;
    damaged.resize(token_at); // First token lacks even a length prefix.
    expect_raw_scope_refused(damaged);
    damaged = raw;
    damaged.resize(token_at + 2U); // Second token length is missing.
    expect_raw_scope_refused(damaged);
    damaged = raw;
    damaged[token_at] = 2;
    damaged.resize(token_at + 2U); // First token has one of its two bytes.
    expect_raw_scope_refused(damaged);
}

TEST(CommandCanonical, decoded_identity_rechecks_scalars_instead_of_trusting_typed_input) {
    // Fixed format order with a one-byte capability id, independent of native layout.
    constexpr uint32_t kind_at = 0;
    constexpr uint32_t revision_at = 4;
    constexpr uint32_t version_at = 8;
    constexpr uint32_t subject_at = 17;
    constexpr uint32_t lifetime_at = 33;
    constexpr uint32_t flags_at = 37;
    constexpr uint32_t duration_at = 41;
    const auto bytes = encoded(request());
    for (const auto &change : std::array<std::pair<uint32_t, uint8_t>, 7>{{{kind_at, 8},
                                                                           {revision_at, 1},
                                                                           {version_at, 0},
                                                                           {subject_at, 0},
                                                                           {lifetime_at, 0},
                                                                           {flags_at + 3U, 0x80},
                                                                           {duration_at, 1}}}) {
        auto damaged = bytes;
        damaged[change.first] = change.second;
        AuthorityCommandRecord output{};
        output.subject_instance_id = 123;
        EXPECT_FALSE(read_authority_command(view(damaged), output)) << change.first;
        EXPECT_EQ(output.subject_instance_id, 123U);
    }
}

TEST(CommandCanonical, pairing_checks_explicit_tokens_units_and_omitted_full_domains_exactly) {
    const ayni_string_v1 all[] = {text("a"), text("b"), text("c")};
    const ayni_string_v1 ab[] = {text("a"), text("b")};
    const ayni_string_v1 ac[] = {text("a"), text("c")};
    const ayni_string_v1 a[] = {text("a")};
    auto field = ayni_authority_field_desc_v1{sizeof(ayni_authority_field_desc_v1), 2,
                                              AYNI_AUTHORITY_PARAMETER, 0, choices(all, 3)};
    field.domain.kind = AYNI_AUTHORITY_ENUM;
    std::array<uint8_t, 256> domain{};
    uint32_t domain_size = 0;
    ASSERT_EQ(ayni::catalog::encode_domain(&field, 1, domain.data(),
                                           static_cast<uint32_t>(domain.size()), domain_size),
              AYNI_OK);
    const ayni_bytes_v1 source{domain.data(), domain_size};
    auto grant = request();
    const auto matches = [&](ayni_authority_value_set_v1 requested,
                             ayni_authority_value_set_v1 stored) {
        ayni_scope_field_v1 constraint{sizeof(constraint), 2, requested};
        grant.scope = {sizeof(grant.scope), 1, &constraint};
        const auto bytes = encoded(grant);
        AuthorityCommandRecord command{};
        EXPECT_TRUE(read_authority_command(view(bytes), command));
        constraint.allowed = stored;
        std::array<uint8_t, 256> scope{};
        uint32_t size = 0;
        EXPECT_EQ(ayni::authority::normalize_scope(source, grant.scope, scope.data(),
                                                   static_cast<uint32_t>(scope.size()), size),
                  AYNI_OK);
        return authority_command_scope_matches(command, source, {scope.data(), size});
    };
    auto two = choices(ab, 2);
    two.kind = AYNI_AUTHORITY_ENUM;
    auto other_two = choices(ac, 2);
    other_two.kind = AYNI_AUTHORITY_ENUM;
    auto one = choices(a, 1);
    one.kind = AYNI_AUTHORITY_ENUM;
    EXPECT_TRUE(matches(two, two));
    EXPECT_FALSE(matches(one, two));
    EXPECT_FALSE(matches(two, other_two));
    EXPECT_FALSE(matches(choices(ab, 2), two)); // Same tokens, different value kind.

    grant.scope = {sizeof(grant.scope), 0, nullptr};
    const auto omitted_bytes = encoded(grant);
    AuthorityCommandRecord omitted{};
    ASSERT_TRUE(read_authority_command(view(omitted_bytes), omitted));
    EXPECT_TRUE(authority_command_scope_matches(omitted, source, source));
    std::array<uint8_t, 256> narrowed{};
    uint32_t size = 0;
    ayni_scope_field_v1 constraint{sizeof(constraint), 2, two};
    grant.scope = {sizeof(grant.scope), 1, &constraint};
    ASSERT_EQ(ayni::authority::normalize_scope(source, grant.scope, narrowed.data(),
                                               static_cast<uint32_t>(narrowed.size()), size),
              AYNI_OK);
    EXPECT_FALSE(authority_command_scope_matches(omitted, source, {narrowed.data(), size}));

    constraint.field_id = 1; // Unknown field before the registered field, not just after it.
    const auto unknown = encoded(grant);
    AuthorityCommandRecord command{};
    ASSERT_TRUE(read_authority_command(view(unknown), command));
    EXPECT_FALSE(authority_command_scope_matches(command, source, source));
    field.domain = number(0, 10);
    ASSERT_EQ(ayni::catalog::encode_domain(&field, 1, domain.data(),
                                           static_cast<uint32_t>(domain.size()), domain_size),
              AYNI_OK);
    const ayni_bytes_v1 numeric_source{domain.data(), domain_size};
    constraint.field_id = 2;
    constraint.allowed = number(0, 10, "s");
    const auto wrong_unit = encoded(grant);
    ASSERT_TRUE(read_authority_command(view(wrong_unit), command));
    EXPECT_FALSE(authority_command_scope_matches(command, numeric_source, numeric_source));
    auto beyond = field;
    beyond.domain = number(0, 11);
    ASSERT_EQ(ayni::catalog::encode_domain(&beyond, 1, narrowed.data(),
                                           static_cast<uint32_t>(narrowed.size()), size),
              AYNI_OK);
    EXPECT_FALSE(authority_command_scope_matches(omitted, numeric_source, {narrowed.data(), size}));
}
} // namespace
