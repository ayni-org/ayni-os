#include "authority/scope/scope.h"
#include "catalog/capability/field_domain.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <vector>

namespace {

using ayni::authority::contains_request;
using ayni::authority::covers;
using ayni::authority::normalize_scope;

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

ayni_authority_field_desc_v1
declaration(uint32_t id, const ayni_authority_value_set_v1 &domain,
            ayni_authority_field_usage_t usage = AYNI_AUTHORITY_PARAMETER) {
    return {sizeof(ayni_authority_field_desc_v1), id, usage, 0, domain};
}

ayni_scope_field_v1 constraint(uint32_t id, const ayni_authority_value_set_v1 &values) {
    return {sizeof(ayni_scope_field_v1), id, values};
}

class Domain {
  public:
    Domain(std::initializer_list<ayni_authority_field_desc_v1> fields) {
        EXPECT_EQ(ayni::catalog::encode_domain(fields.begin(), static_cast<uint32_t>(fields.size()),
                                               data_.data(), static_cast<uint32_t>(data_.size()),
                                               size_),
                  AYNI_OK);
    }
    ayni_bytes_v1 view() const { return {data_.data(), size_}; }

  private:
    std::array<uint8_t, AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX> data_{};
    uint32_t size_ = 0;
};

struct Input {
    Input(std::initializer_list<ayni_scope_field_v1> values) : fields(values) {}
    ayni_scope_v1 view() const {
        return {sizeof(ayni_scope_v1), static_cast<uint32_t>(fields.size()), fields.data()};
    }
    std::vector<ayni_scope_field_v1> fields;
};

std::vector<uint8_t> normalized(const Domain &domain, const Input &input) {
    uint32_t size = 0;
    EXPECT_EQ(normalize_scope(domain.view(), input.view(), nullptr, 0, size),
              AYNI_BUFFER_TOO_SMALL);
    std::vector<uint8_t> bytes(size);
    EXPECT_EQ(normalize_scope(domain.view(), input.view(), bytes.data(),
                              static_cast<uint32_t>(bytes.size()), size),
              AYNI_OK);
    return bytes;
}

ayni_bytes_v1 view(const std::vector<uint8_t> &bytes) {
    return {bytes.data(), static_cast<uint32_t>(bytes.size())};
}

TEST(Scope, omitted_fields_and_explicit_whole_domain_have_identical_canonical_bytes) {
    const ayni_string_v1 choices[] = {token("west"), token("east")};
    Domain domain{declaration(7, integer(-3, 9)),
                  declaration(2, tokens(AYNI_AUTHORITY_ENUM, choices, 2))};
    const Input omitted{};
    const Input explicit_domain{constraint(7, integer(-3, 9)),
                                constraint(2, tokens(AYNI_AUTHORITY_ENUM, choices, 2))};
    const auto implicit = normalized(domain, omitted);
    EXPECT_EQ(implicit, normalized(domain, explicit_domain));
    EXPECT_EQ(implicit,
              (std::vector<uint8_t>{domain.view().data, domain.view().data + domain.view().size}));
}

TEST(Scope, numeric_encoding_is_versioned_fixed_width_little_endian_twos_complement) {
    Domain domain{declaration(3, integer(-9, 9, "ms"))};
    const Input scope{constraint(3, integer(-1, 2, "ms"))};
    const std::vector<uint8_t> expected{1,   0,   1,   0,   3,   0,   0,   0,   1,   1,
                                        2,   0,   'm', 's', 255, 255, 255, 255, 255, 255,
                                        255, 255, 2,   0,   0,   0,   0,   0,   0,   0};
    EXPECT_EQ(normalized(domain, scope), expected);
}

TEST(Scope, reordered_fields_and_duplicate_token_members_produce_the_same_canonical_body) {
    const ayni_string_v1 whole[] = {token("west"), token("east"), token("north")};
    const ayni_string_v1 unordered[] = {token("west"), token("east"), token("west")};
    const ayni_string_v1 ordered[] = {token("east"), token("west")};
    Domain domain{declaration(1, integer(0, 10)),
                  declaration(2, tokens(AYNI_AUTHORITY_IDENTIFIER, whole, 3))};
    const Input first{constraint(2, tokens(AYNI_AUTHORITY_IDENTIFIER, unordered, 3)),
                      constraint(1, integer(1, 4))};
    const Input second{constraint(1, integer(1, 4)),
                       constraint(2, tokens(AYNI_AUTHORITY_IDENTIFIER, ordered, 2))};
    EXPECT_EQ(normalized(domain, first), normalized(domain, second));
}

TEST(Scope, numeric_coverage_matches_an_independent_exhaustive_point_oracle) {
    Domain domain{declaration(1, integer(-3, 3))};
    for (int parent_min = -3; parent_min <= 3; ++parent_min) {
        for (int parent_max = parent_min; parent_max <= 3; ++parent_max) {
            const Input parent{constraint(1, integer(parent_min, parent_max))};
            for (int child_min = -3; child_min <= 3; ++child_min) {
                for (int child_max = child_min; child_max <= 3; ++child_max) {
                    const Input child{constraint(1, integer(child_min, child_max))};
                    bool oracle = true;
                    for (int point = -3; point <= 3; ++point) {
                        if (point >= child_min && point <= child_max &&
                            !(point >= parent_min && point <= parent_max)) {
                            oracle = false;
                        }
                    }
                    const auto result = covers(domain.view(), parent.view(), child.view());
                    ASSERT_EQ(result.status, AYNI_OK);
                    EXPECT_EQ(result.covered, oracle) << parent_min << ',' << parent_max << " / "
                                                      << child_min << ',' << child_max;
                }
            }
        }
    }
}

TEST(Scope, token_coverage_matches_an_independent_bitset_oracle_for_every_nonempty_subset) {
    const ayni_string_v1 whole[] = {token("east"), token("north"), token("west")};
    Domain domain{declaration(1, tokens(AYNI_AUTHORITY_ENUM, whole, 3))};
    for (uint32_t parent_mask = 1; parent_mask < 8; ++parent_mask) {
        for (uint32_t child_mask = 1; child_mask < 8; ++child_mask) {
            std::array<ayni_string_v1, 3> parent_members{};
            std::array<ayni_string_v1, 3> child_members{};
            uint32_t parent_count = 0;
            uint32_t child_count = 0;
            for (uint32_t bit = 0; bit < 3; ++bit) {
                if ((parent_mask & (1U << bit)) != 0U) {
                    parent_members[parent_count++] = whole[bit];
                }
                if ((child_mask & (1U << bit)) != 0U) {
                    child_members[child_count++] = whole[bit];
                }
            }
            const Input parent{
                constraint(1, tokens(AYNI_AUTHORITY_ENUM, parent_members.data(), parent_count))};
            const Input child{
                constraint(1, tokens(AYNI_AUTHORITY_ENUM, child_members.data(), child_count))};
            const auto result = covers(domain.view(), parent.view(), child.view());
            ASSERT_EQ(result.status, AYNI_OK);
            EXPECT_EQ(result.covered, (child_mask & ~parent_mask) == 0U);
        }
    }
}

TEST(Scope, signed_extremes_singletons_reflexivity_and_transitivity_do_not_overflow) {
    const auto lowest = std::numeric_limits<int64_t>::min();
    const auto highest = std::numeric_limits<int64_t>::max();
    Domain domain{declaration(1, integer(lowest, highest))};
    const Input full{};
    const Input middle{constraint(1, integer(lowest + 1, highest - 1))};
    const Input narrow{constraint(1, integer(-1, 1))};
    EXPECT_TRUE(covers(domain.view(), full.view(), middle.view()).covered);
    EXPECT_TRUE(covers(domain.view(), middle.view(), narrow.view()).covered);
    EXPECT_TRUE(covers(domain.view(), full.view(), narrow.view()).covered);
    EXPECT_TRUE(covers(domain.view(), narrow.view(), narrow.view()).covered);
    for (const auto point : {lowest, highest}) {
        const Input actual{constraint(1, integer(point, point))};
        const auto allowed = contains_request(domain.view(), full.view(), actual.view());
        EXPECT_EQ(allowed.status, AYNI_OK);
        EXPECT_TRUE(allowed.covered);
        EXPECT_FALSE(contains_request(domain.view(), middle.view(), actual.view()).covered);
    }
}

TEST(Scope, valid_noncoverage_is_distinct_from_malformed_or_out_of_domain_input) {
    Domain domain{declaration(1, integer(0, 10))};
    const Input allowed{constraint(1, integer(2, 4))};
    const Input outside_allowed{constraint(1, integer(3, 5))};
    const auto result = covers(domain.view(), allowed.view(), outside_allowed.view());
    EXPECT_EQ(result.status, AYNI_OK);
    EXPECT_FALSE(result.covered);
    for (const Input &bad :
         {Input{constraint(1, integer(-1, 1))}, Input{constraint(1, integer(5, 4))},
          Input{constraint(1, integer(2, 4, "ms"))}, Input{constraint(9, integer(2, 4))}}) {
        const auto rejected = covers(domain.view(), allowed.view(), bad.view());
        EXPECT_EQ(rejected.status, AYNI_INVALID_ARGUMENT);
        EXPECT_FALSE(rejected.covered);
    }
}

TEST(Scope, actual_request_requires_every_field_exactly_once_and_a_single_value_per_field) {
    Domain domain{declaration(1, integer(0, 10)), declaration(2, integer(0, 10))};
    const Input allowed{};
    const Input missing{constraint(1, integer(2, 2))};
    const Input duplicate{constraint(1, integer(2, 2)), constraint(1, integer(3, 3))};
    const Input interval{constraint(1, integer(2, 3)), constraint(2, integer(4, 4))};
    const Input complete{constraint(2, integer(4, 4)), constraint(1, integer(2, 2))};
    EXPECT_EQ(contains_request(domain.view(), allowed.view(), missing.view()).status,
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(contains_request(domain.view(), allowed.view(), duplicate.view()).status,
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(contains_request(domain.view(), allowed.view(), interval.view()).status,
              AYNI_INVALID_ARGUMENT);
    EXPECT_TRUE(contains_request(domain.view(), allowed.view(), complete.view()).covered);
}

TEST(Scope, enum_and_identifier_types_cannot_be_substituted_or_given_unknown_members) {
    const ayni_string_v1 whole[] = {token("east"), token("west")};
    const ayni_string_v1 unknown[] = {token("north")};
    Domain domain{declaration(1, tokens(AYNI_AUTHORITY_ENUM, whole, 2))};
    const Input allowed{};
    const Input wrong_kind{constraint(1, tokens(AYNI_AUTHORITY_IDENTIFIER, whole, 2))};
    const Input wrong_member{constraint(1, tokens(AYNI_AUTHORITY_ENUM, unknown, 1))};
    const Input multiple_actual{constraint(1, tokens(AYNI_AUTHORITY_ENUM, whole, 2))};
    EXPECT_EQ(covers(domain.view(), allowed.view(), wrong_kind.view()).status,
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(covers(domain.view(), allowed.view(), wrong_member.view()).status,
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(contains_request(domain.view(), allowed.view(), multiple_actual.view()).status,
              AYNI_INVALID_ARGUMENT);
}

TEST(Scope, targets_and_egress_use_exact_identifiers_and_cannot_authorize_one_another) {
    const ayni_string_v1 endpoints[] = {token("arm"), token("arm2")};
    const ayni_string_v1 arm[] = {token("arm")};
    const ayni_string_v1 arm2[] = {token("arm2")};
    Domain domain{declaration(1, tokens(AYNI_AUTHORITY_IDENTIFIER, endpoints, 2),
                              AYNI_AUTHORITY_EXECUTION_TARGET),
                  declaration(2, tokens(AYNI_AUTHORITY_IDENTIFIER, endpoints, 2),
                              AYNI_AUTHORITY_EGRESS_DESTINATION)};
    const Input allowed{constraint(1, tokens(AYNI_AUTHORITY_IDENTIFIER, arm, 1)),
                        constraint(2, tokens(AYNI_AUTHORITY_IDENTIFIER, arm2, 1))};
    const Input swapped{constraint(1, tokens(AYNI_AUTHORITY_IDENTIFIER, arm2, 1)),
                        constraint(2, tokens(AYNI_AUTHORITY_IDENTIFIER, arm, 1))};
    const auto result = contains_request(domain.view(), allowed.view(), swapped.view());
    EXPECT_EQ(result.status, AYNI_OK);
    EXPECT_FALSE(result.covered);
    EXPECT_TRUE(contains_request(domain.view(), allowed.view(), allowed.view()).covered);
}

TEST(Scope, malformed_input_does_not_change_output_and_short_buffer_reports_only_required_size) {
    Domain domain{declaration(1, integer(0, 10)), declaration(2, integer(0, 10))};
    const Input malformed{constraint(1, integer(1, 2)), constraint(2, integer(3, 4, "ms"))};
    std::array<uint8_t, 128> bytes{};
    bytes.fill(0xAA);
    const auto before = bytes;
    uint32_t size = 91;
    EXPECT_EQ(normalize_scope(domain.view(), malformed.view(), bytes.data(),
                              static_cast<uint32_t>(bytes.size()), size),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(bytes, before);
    EXPECT_EQ(size, 91U);
    const Input full{};
    EXPECT_EQ(
        normalize_scope(domain.view(), full.view(), bytes.data(), domain.view().size - 1U, size),
        AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(size, domain.view().size);
    EXPECT_EQ(bytes, before);
    size = 91;
    EXPECT_EQ(normalize_scope(domain.view(), full.view(), nullptr, 128, size),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(size, 91U);
}

TEST(Scope, malformed_scope_form_and_newer_nested_structs_keep_classified_errors) {
    Domain domain{declaration(1, integer(0, 10))};
    const Input valid{constraint(1, integer(1, 1))};
    auto scope = valid.view();
    scope.field_count = 9;
    scope.fields = nullptr;
    EXPECT_EQ(covers(domain.view(), scope, valid.view()).status, AYNI_INVALID_ARGUMENT);
    scope = valid.view();
    ++scope.struct_size;
    EXPECT_EQ(covers(domain.view(), scope, valid.view()).status, AYNI_UNSUPPORTED_VERSION);
    Input newer{constraint(1, integer(1, 1))};
    ++newer.fields[0].struct_size;
    EXPECT_EQ(covers(domain.view(), newer.view(), valid.view()).status, AYNI_UNSUPPORTED_VERSION);
    newer.fields[0].struct_size = sizeof(ayni_scope_field_v1);
    ++newer.fields[0].allowed.struct_size;
    EXPECT_EQ(covers(domain.view(), newer.view(), valid.view()).status, AYNI_UNSUPPORTED_VERSION);
}

TEST(Scope, canonical_scope_records_are_validated_before_encoded_containment) {
    Domain domain{declaration(1, integer(-10, 10))};
    const Input parent{constraint(1, integer(-5, 5))};
    const Input child{constraint(1, integer(-2, 2))};
    const auto parent_bytes = normalized(domain, parent);
    const auto child_bytes = normalized(domain, child);
    EXPECT_TRUE(covers(domain.view(), view(parent_bytes), view(child_bytes)).covered);
    EXPECT_FALSE(covers(domain.view(), view(child_bytes), view(parent_bytes)).covered);
    const Input actual{constraint(1, integer(5, 5))};
    EXPECT_TRUE(contains_request(domain.view(), view(parent_bytes), actual.view()).covered);
    for (uint32_t size = 0; size < parent_bytes.size(); ++size) {
        EXPECT_EQ(covers(domain.view(), {parent_bytes.data(), size}, view(child_bytes)).status,
                  AYNI_INVALID_ARGUMENT)
            << size;
    }
    auto damaged = parent_bytes;
    damaged.push_back(0);
    EXPECT_EQ(covers(domain.view(), view(damaged), view(child_bytes)).status,
              AYNI_INVALID_ARGUMENT);
    damaged = parent_bytes;
    damaged[3] = 1;
    EXPECT_EQ(covers(domain.view(), view(damaged), view(child_bytes)).status,
              AYNI_INVALID_ARGUMENT);
    damaged = parent_bytes;
    damaged[0] = 2;
    EXPECT_EQ(covers(domain.view(), view(damaged), view(child_bytes)).status,
              AYNI_INVALID_ARGUMENT);
    damaged = parent_bytes;
    damaged[9] = AYNI_AUTHORITY_EGRESS_DESTINATION;
    EXPECT_EQ(covers(domain.view(), view(damaged), view(child_bytes)).status,
              AYNI_INVALID_ARGUMENT);
}

TEST(Scope, decoding_and_normalizing_a_canonical_body_is_idempotent) {
    const ayni_string_v1 whole[] = {token("west"), token("east")};
    const ayni_string_v1 chosen[] = {token("east")};
    Domain domain{declaration(1, integer(-10, 10)),
                  declaration(2, tokens(AYNI_AUTHORITY_ENUM, whole, 2))};
    const Input input{constraint(2, tokens(AYNI_AUTHORITY_ENUM, chosen, 1)),
                      constraint(1, integer(0, 2))};
    const auto bytes = normalized(domain, input);
    ayni::catalog::DomainReader reader(view(bytes));
    std::array<ayni::catalog::DomainField, 8> decoded{};
    std::array<ayni_scope_field_v1, 8> fields{};
    uint32_t count = 0;
    while (count < decoded.size() && reader.next(decoded[count])) {
        const auto &values = decoded[count].values;
        auto value = values.kind == AYNI_AUTHORITY_I64
                         ? integer(values.minimum, values.maximum)
                         : tokens(values.kind, values.tokens.data(), values.token_count);
        value.unit = values.unit;
        fields[count] = constraint(decoded[count].id, value);
        ++count;
    }
    ASSERT_TRUE(reader.ok());
    const ayni_scope_v1 input_again{sizeof(ayni_scope_v1), count, fields.data()};
    std::array<uint8_t, 256> output{};
    uint32_t size = 0;
    ASSERT_EQ(normalize_scope(domain.view(), input_again, output.data(),
                              static_cast<uint32_t>(output.size()), size),
              AYNI_OK);
    EXPECT_EQ((std::vector<uint8_t>{output.data(), output.data() + size}), bytes);
}

TEST(Scope, no_field_capability_accepts_only_an_empty_actual_request) {
    Domain domain{};
    const Input empty{};
    EXPECT_TRUE(contains_request(domain.view(), empty.view(), empty.view()).covered);
    const Input extra{constraint(1, integer(1, 1))};
    EXPECT_EQ(contains_request(domain.view(), empty.view(), extra.view()).status,
              AYNI_INVALID_ARGUMENT);
}

// Independent format-1 byte fixtures bypass typed normalization: stored
// canonical bodies must reject malformed ordering instead of repairing it.
TEST(Scope, canonical_reader_rejects_zero_duplicate_and_descending_field_ids) {
    const std::vector<uint8_t> valid{1, 0,   2, 0, 1, 0, 0, 0, 2, 1, 0, 1,
                                     1, 'a', 2, 0, 0, 0, 2, 1, 0, 1, 1, 'a'};
    ayni::catalog::DomainField field{};
    ayni::catalog::DomainReader positive(view(valid));
    ASSERT_TRUE(positive.next(field));
    ASSERT_TRUE(positive.next(field));
    ASSERT_TRUE(positive.ok());
    // Literal fixture offsets are independent of production constants.
    constexpr uint32_t first_id = 4;
    constexpr uint32_t second_id = 14;
    for (unsigned malformed = 0; malformed < 3; ++malformed) {
        SCOPED_TRACE(malformed);
        auto bytes = valid;
        if (malformed == 0U) {
            bytes[first_id] = 0;
        } else {
            bytes[second_id] = 1;
            if (malformed == 2U) {
                bytes[first_id] = 2;
            }
        }
        ayni::catalog::DomainReader reader(view(bytes));
        uint32_t count = 0;
        while (reader.next(field)) {
            ++count;
        }
        EXPECT_FALSE(reader.ok());
        EXPECT_LE(count, 1U);
    }
}

TEST(Scope, encoded_duplicate_and_descending_members_are_errors_for_every_consumer) {
    for (const auto kind : {AYNI_AUTHORITY_ENUM, AYNI_AUTHORITY_IDENTIFIER}) {
        SCOPED_TRACE(kind);
        const std::vector<uint8_t> valid{1, 0, 1, 0, 1,   0, 0,  0, static_cast<uint8_t>(kind),
                                         1, 0, 2, 1, 'a', 1, 'b'};
        const ayni_string_v1 selected[] = {token("b")};
        const Input actual{constraint(1, tokens(kind, selected, 1))};
        ASSERT_TRUE(covers(view(valid), view(valid), view(valid)).covered);
        ASSERT_TRUE(contains_request(view(valid), view(valid), actual.view()).covered);
        for (const bool descending : {false, true}) {
            SCOPED_TRACE(descending);
            auto bytes = valid;
            bytes[13] = 'b';
            bytes[15] = descending ? 'a' : 'b';
            ayni::catalog::DomainReader reader(view(bytes));
            ayni::catalog::DomainField field{};
            EXPECT_FALSE(reader.next(field));
            EXPECT_FALSE(reader.ok());
            EXPECT_EQ(covers(view(valid), view(valid), view(bytes)).status, AYNI_INVALID_ARGUMENT);
            EXPECT_EQ(covers(view(valid), view(bytes), view(valid)).status, AYNI_INVALID_ARGUMENT);
            EXPECT_EQ(contains_request(view(valid), view(bytes), actual.view()).status,
                      AYNI_INVALID_ARGUMENT);
        }
    }
}

TEST(Scope, encoded_single_point_integer_domains_and_constraints_remain_valid_at_extremes) {
    const auto low = std::numeric_limits<int64_t>::min();
    const auto high = std::numeric_limits<int64_t>::max();
    Domain whole{declaration(1, integer(low, high))};
    const auto whole_body = normalized(whole, Input{});
    for (const int64_t point : {low, int64_t{0}, high}) {
        SCOPED_TRACE(point);
        Domain singleton{declaration(1, integer(point, point))};
        ayni::catalog::DomainReader reader(singleton.view());
        ayni::catalog::DomainField field{};
        ASSERT_TRUE(reader.next(field));
        EXPECT_EQ(field.values.minimum, point);
        EXPECT_EQ(field.values.maximum, point);
        EXPECT_TRUE(reader.ok());
        const Input selected{constraint(1, integer(point, point))};
        const auto selected_body = normalized(whole, selected);
        EXPECT_TRUE(covers(whole.view(), view(whole_body), view(selected_body)).covered);
        EXPECT_FALSE(covers(whole.view(), view(selected_body), view(whole_body)).covered);
        EXPECT_TRUE(contains_request(whole.view(), view(selected_body), selected.view()).covered);
        EXPECT_TRUE(contains_request(singleton.view(), selected.view(), selected.view()).covered);
    }
}

// Both uses accept IDENTIFIER values. Keep those values identical so a
// changed use cannot be rejected accidentally by a type or value check.
TEST(Scope, encoded_identifier_scopes_cannot_exchange_target_and_egress_usages) {
    const ayni_string_v1 members[] = {token("shared")};
    const auto values = tokens(AYNI_AUTHORITY_IDENTIFIER, members, 1);
    Domain domain{declaration(1, values, AYNI_AUTHORITY_EXECUTION_TARGET),
                  declaration(2, values, AYNI_AUTHORITY_EGRESS_DESTINATION)};
    const Input actual{constraint(1, values), constraint(2, values)};
    const auto canonical = normalized(domain, actual);
    EXPECT_TRUE(covers(domain.view(), view(canonical), view(canonical)).covered);
    EXPECT_TRUE(contains_request(domain.view(), view(canonical), actual.view()).covered);

    ayni::catalog::DomainReader original(view(canonical));
    std::array<ayni::catalog::DomainField, 2> fields{};
    ASSERT_TRUE(original.next(fields[0]));
    ASSERT_TRUE(original.next(fields[1]));
    ASSERT_TRUE(original.ok());
    fields[0].usage = AYNI_AUTHORITY_EGRESS_DESTINATION;
    fields[1].usage = AYNI_AUTHORITY_EXECUTION_TARGET;
    std::vector<uint8_t> exchanged(canonical.size());
    ayni::catalog::write_domain_header(exchanged.data(), static_cast<uint32_t>(fields.size()));
    uint32_t offset = ayni::catalog::field_domain_header_size;
    for (const auto &field : fields) {
        ayni::catalog::write_domain_field(field, exchanged.data() + offset);
        offset += ayni::catalog::encoded_field_size(field);
    }
    ASSERT_EQ(offset, exchanged.size());

    // The exchanged record remains canonical in isolation; only its
    // disagreement with the registered schema must reject authorization.
    ayni::catalog::DomainReader exchanged_reader(view(exchanged));
    ayni::catalog::DomainField decoded{};
    ASSERT_TRUE(exchanged_reader.next(decoded));
    EXPECT_EQ(decoded.usage, AYNI_AUTHORITY_EGRESS_DESTINATION);
    ASSERT_TRUE(exchanged_reader.next(decoded));
    EXPECT_EQ(decoded.usage, AYNI_AUTHORITY_EXECUTION_TARGET);
    ASSERT_TRUE(exchanged_reader.ok());
    EXPECT_EQ(covers(domain.view(), view(exchanged), view(canonical)).status,
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(covers(domain.view(), view(canonical), view(exchanged)).status,
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(contains_request(domain.view(), view(exchanged), actual.view()).status,
              AYNI_INVALID_ARGUMENT);
}

// C-AUTH-018: stored constraints still require every concrete request field
// to be present and single-valued. Legal noncoverage remains OK/false.
TEST(Scope, stored_allowed_scope_requires_a_complete_single_value_request) {
    const ayni_string_v1 choices[] = {token("east"), token("west")};
    Domain domain{declaration(1, integer(0, 10)),
                  declaration(2, tokens(AYNI_AUTHORITY_ENUM, choices, 2))};
    const Input allowed{constraint(1, integer(2, 4))};
    const auto stored = normalized(domain, allowed);
    const auto east = tokens(AYNI_AUTHORITY_ENUM, choices, 1);
    const Input missing{constraint(1, integer(2, 2))};
    const Input interval{constraint(1, integer(2, 3)), constraint(2, east)};
    const Input set{constraint(1, integer(2, 2)),
                    constraint(2, tokens(AYNI_AUTHORITY_ENUM, choices, 2))};
    for (const Input *input : {&missing, &interval, &set}) {
        const auto result = contains_request(domain.view(), view(stored), input->view());
        EXPECT_EQ(result.status, AYNI_INVALID_ARGUMENT);
        EXPECT_FALSE(result.covered);
    }
    const Input complete{constraint(2, east), constraint(1, integer(2, 2))};
    const auto accepted = contains_request(domain.view(), view(stored), complete.view());
    EXPECT_EQ(accepted.status, AYNI_OK);
    EXPECT_TRUE(accepted.covered);
    const Input outside{constraint(2, east), constraint(1, integer(5, 5))};
    const auto denied = contains_request(domain.view(), view(stored), outside.view());
    EXPECT_EQ(denied.status, AYNI_OK);
    EXPECT_FALSE(denied.covered);
}

// ADR-0034 decision 4 / C-VER-003: validate the full parent before the child,
// including when nested values and the opposite top-level header both fail.
TEST(Scope, simultaneous_operand_errors_preserve_parent_before_child_classification) {
    Domain domain{declaration(1, integer(0, 10))};
    Input parent{constraint(1, integer(2, 4))};
    Input child{constraint(1, integer(3, 3))};
    parent.fields[0].allowed.values.integer = {4, 2};
    auto newer_child = child.view();
    ++newer_child.struct_size;
    EXPECT_EQ(covers(domain.view(), parent.view(), newer_child).status, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(contains_request(domain.view(), parent.view(), newer_child).status,
              AYNI_INVALID_ARGUMENT);
    parent.fields[0].allowed = integer(2, 4);
    ++parent.fields[0].allowed.struct_size;
    child.fields[0].field_id = 0;
    EXPECT_EQ(covers(domain.view(), parent.view(), child.view()).status, AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(contains_request(domain.view(), parent.view(), child.view()).status,
              AYNI_UNSUPPORTED_VERSION);
}

} // namespace
