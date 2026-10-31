#include "catalog/capability/value_encoding.h"
#include "workspace_fixture.h"
#include <ayni/values.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
ayni_value_node_v1 integer(uint32_t field, uint32_t end, int64_t value) {
    ayni_value_node_v1 node{};
    node.kind = AYNI_VALUE_I64;
    node.field_id = field;
    node.subtree_end = end;
    node.value.integer = value;
    return node;
}

// ADR-0084: exact decimal parsing never converts through binary floating point.
TEST(BusinessValues, decimal_keeps_scale_and_signed_extremes_without_rounding) {
    ayni_decimal_v1 value{91, 1, 0};
    ASSERT_EQ(ayni_decimal_parse({"1.200", 5}, 2, &value), AYNI_OK);
    EXPECT_EQ(value.coefficient, 120);
    EXPECT_EQ(value.scale, 2U);
    ASSERT_EQ(ayni_decimal_parse({"-9223372036854775808", 20}, 0, &value), AYNI_OK);
    EXPECT_EQ(value.coefficient, std::numeric_limits<int64_t>::min());
    for (const char *text : {"1.201", "1e2", "+1", "1.", ".5", "9223372036854775808"}) {
        SCOPED_TRACE(text);
        const auto before = value;
        EXPECT_EQ(ayni_decimal_parse({text, static_cast<uint32_t>(std::strlen(text))}, 2, &value),
                  AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(value.coefficient, before.coefficient);
        EXPECT_EQ(value.scale, before.scale);
    }
}

// ADR-0084: field order is canonical, defaults precede authorization and a
// missing optional without a default remains absent; list order is unrelated.
TEST(BusinessValues, object_fields_sort_by_id_and_static_defaults_fill_missing_values) {
    auto default_node = integer(0, 1, 7);
    ayni_value_v1 default_value{&default_node, 1};
    std::array<ayni_schema_node_v1, 4> schema{};
    schema[0].kind = AYNI_VALUE_OBJECT;
    schema[0].subtree_end = static_cast<uint32_t>(schema.size());
    for (uint32_t at = 1; at < schema.size(); ++at) {
        schema[at].kind = AYNI_VALUE_I64;
        schema[at].field_id = at;
        schema[at].subtree_end = at + 1U;
        schema[at].name = at == 1U   ? ayni_string_v1{"first", 5}
                          : at == 2U ? ayni_string_v1{"second", 6}
                                     : ayni_string_v1{"optional", 8};
        schema[at].min_i64 = 0;
        schema[at].max_i64 = 10;
    }
    schema[1].default_value = &default_value;
    schema[2].flags = AYNI_SCHEMA_REQUIRED;
    ayni_schema_v1 declaration{schema.data(), static_cast<uint32_t>(schema.size())};
    std::array<ayni_value_node_v1, 2> input{};
    input[0].kind = AYNI_VALUE_OBJECT;
    input[0].subtree_end = static_cast<uint32_t>(input.size());
    input[1] = integer(2, 2, 4);
    ayni_value_v1 value{input.data(), static_cast<uint32_t>(input.size())};
    std::array<ayni_value_node_v1, 4> nodes{};
    ayni_value_buffer_v1 output{
        nodes.data(), static_cast<uint32_t>(nodes.size()), 0, nullptr, 0, 0};
    ASSERT_EQ(ayni_value_normalize(::ayni::test::value_workspace(), &declaration, &value, &output),
              AYNI_OK);
    EXPECT_EQ(output.node_count, 3U);
    EXPECT_EQ(nodes[0].subtree_end, 3U);
    EXPECT_EQ(nodes[1].field_id, 1U);
    EXPECT_EQ(nodes[1].value.integer, 7);
    EXPECT_EQ(nodes[2].field_id, 2U);
    EXPECT_EQ(nodes[2].value.integer, 4);
    default_node.value.integer = 11;
    EXPECT_EQ(ayni_schema_validate(::ayni::test::value_workspace(), &declaration),
              AYNI_INVALID_ARGUMENT);
}

// ADR-0084: capacity preflight reports complete defaults without publishing
// a partial tree; semantic refusal preserves the caller's complete output.
TEST(BusinessValues, small_output_reports_requirements_without_touching_buffers) {
    ayni_schema_node_v1 field{};
    field.kind = AYNI_VALUE_TEXT;
    field.subtree_end = 1;
    field.max_count = 16;
    ayni_schema_v1 schema{&field, 1};
    ayni_value_node_v1 node{};
    node.kind = AYNI_VALUE_TEXT;
    node.subtree_end = 1;
    node.value.text = {"hello", 5};
    ayni_value_v1 value{&node, 1};
    ayni_value_node_v1 sentinel = integer(99, 1, 91);
    std::array<char, 4> bytes{'k', 'e', 'e', 'p'};
    ayni_value_buffer_v1 output{&sentinel, 1, 19, bytes.data(), 4, 23};
    ASSERT_EQ(ayni_value_normalize(::ayni::test::value_workspace(), &schema, &value, &output),
              AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(output.node_count, 1U);
    EXPECT_EQ(output.byte_count, 5U);
    EXPECT_EQ(sentinel.value.integer, 91);
    EXPECT_EQ(std::string(bytes.data(), bytes.size()), "keep");
    field.max_count = 2;
    output.node_count = 19;
    output.byte_count = 23;
    EXPECT_EQ(ayni_value_normalize(::ayni::test::value_workspace(), &schema, &value, &output),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(output.node_count, 19U);
    EXPECT_EQ(output.byte_count, 23U);
}

// ADR-0084: lists preserve duplicate elements and scalar union admission reads
// only the active member. Negative float zero has one canonical value.
TEST(BusinessValues, lists_preserve_order_duplicates_and_canonicalize_negative_zero) {
    std::array<ayni_schema_node_v1, 2> nodes{};
    nodes[0].kind = AYNI_VALUE_LIST;
    nodes[0].subtree_end = 2;
    nodes[0].max_count = 4;
    nodes[1].kind = AYNI_VALUE_FLOAT64;
    nodes[1].subtree_end = 2;
    nodes[1].min_float64 = -1;
    nodes[1].max_float64 = 1;
    ayni_schema_v1 schema{nodes.data(), static_cast<uint32_t>(nodes.size())};
    std::array<ayni_value_node_v1, 4> input{};
    input[0].kind = AYNI_VALUE_LIST;
    input[0].subtree_end = static_cast<uint32_t>(input.size());
    for (uint32_t at = 1; at < input.size(); ++at) {
        input[at].kind = AYNI_VALUE_FLOAT64;
        input[at].subtree_end = at + 1U;
        input[at].value.approximate = at == 1U ? -0.0 : 0.5;
    }
    ayni_value_v1 value{input.data(), static_cast<uint32_t>(input.size())};
    std::array<ayni_value_node_v1, 4> result{};
    ayni_value_buffer_v1 output{
        result.data(), static_cast<uint32_t>(result.size()), 0, nullptr, 0, 0};
    ASSERT_EQ(ayni_value_normalize(::ayni::test::value_workspace(), &schema, &value, &output),
              AYNI_OK);
    EXPECT_FALSE(std::signbit(result[1].value.approximate));
    EXPECT_DOUBLE_EQ(result[2].value.approximate, 0.5);
    EXPECT_DOUBLE_EQ(result[3].value.approximate, 0.5);
    input[1].value.approximate = std::numeric_limits<double>::infinity();
    EXPECT_EQ(ayni_value_validate(&value), AYNI_INVALID_ARGUMENT);
}

// ADR-0084: closed objects reject unknown, duplicate and crossing fields;
// required/nullable are separate, and an invalid default never becomes data.
TEST(BusinessValues, malformed_tree_and_unknown_fields_are_rejected) {
    std::array<ayni_value_node_v1, 3> nodes{};
    nodes[0].kind = AYNI_VALUE_OBJECT;
    nodes[0].subtree_end = 3;
    nodes[1] = integer(1, 2, 0);
    nodes[2] = integer(1, 3, 0);
    ayni_value_v1 value{nodes.data(), static_cast<uint32_t>(nodes.size())};
    EXPECT_EQ(ayni_value_validate(&value), AYNI_INVALID_ARGUMENT);
    nodes[2].field_id = 2;
    ASSERT_EQ(ayni_value_validate(&value), AYNI_OK);
    nodes[1].subtree_end = 3;
    EXPECT_EQ(ayni_value_validate(&value), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_value_validate(nullptr), AYNI_INVALID_ARGUMENT);
}

// ADR-0084: persistent representations encode active members rather than ABI
// structs. Every short body is an ordinary input-boundary refusal, not replay evidence.
TEST(BusinessValues, portable_value_encoding_round_trips_and_rejects_trailing_or_short_bodies) {
    std::array<ayni_value_node_v1, 3> nodes{};
    nodes[0].kind = AYNI_VALUE_OBJECT;
    nodes[0].subtree_end = static_cast<uint32_t>(nodes.size());
    nodes[1] = integer(1, 2, std::numeric_limits<int64_t>::min());
    nodes[2].kind = AYNI_VALUE_TEXT;
    nodes[2].field_id = 2;
    nodes[2].subtree_end = 3;
    nodes[2].value.text = {"world 世界", 12};
    ayni_value_v1 value{nodes.data(), static_cast<uint32_t>(nodes.size())};
    std::array<uint8_t, 128> bytes{};
    uint32_t used = 91;
    ASSERT_EQ(ayni::catalog::encode_value(value, nullptr, 0, used), AYNI_BUFFER_TOO_SMALL);
    EXPECT_GT(used, 0U);
    ASSERT_EQ(
        ayni::catalog::encode_value(value, bytes.data(), static_cast<uint32_t>(bytes.size()), used),
        AYNI_OK);
    ayni::catalog::DecodedValue decoded;
    ASSERT_EQ(
        ayni::catalog::decode_value(::ayni::test::codec_workspace(), {bytes.data(), used}, decoded),
        AYNI_OK);
    EXPECT_TRUE(ayni::catalog::canonical_value(decoded.view()));
    EXPECT_EQ(decoded.nodes[1].value.integer, std::numeric_limits<int64_t>::min());
    EXPECT_EQ(std::string(decoded.nodes[2].value.text.data, decoded.nodes[2].value.text.size),
              "world 世界");
    for (uint32_t size = 0; size < used; ++size) {
        SCOPED_TRACE(size);
        decoded.count = 91;
        EXPECT_EQ(ayni::catalog::decode_value(::ayni::test::codec_workspace(), {bytes.data(), size},
                                              decoded),
                  AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(decoded.count, 91U);
    }
    EXPECT_EQ(ayni::catalog::decode_value(::ayni::test::codec_workspace(),
                                          {bytes.data(), used + 1U}, decoded),
              AYNI_INVALID_ARGUMENT);
}
} // namespace
