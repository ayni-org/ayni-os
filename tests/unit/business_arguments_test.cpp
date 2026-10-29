// ADR-0084: adapters preserve names and numeric lexemes until the original
// Schema resolves them; the native normalization/default owner remains shared.
#include "catalog/capability/argument_encoding.h"
#include <ayni/arguments.h>
#include <ayni/values.hpp>
#include <ayni/values_json.hpp>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
ayni_string_v1 spelling(const char *text) {
    return {text, static_cast<uint32_t>(std::strlen(text))};
}

struct Output {
    std::array<ayni_value_node_v1, 16> nodes{};
    std::array<char, 128> bytes{};
    ayni_value_buffer_v1 view{nodes.data(), static_cast<uint32_t>(nodes.size()), 0,
                              bytes.data(), static_cast<uint32_t>(bytes.size()), 0};
};

// ADR-0084 precise-number representation: a wide Schema always uses strings,
// including values that would individually fit a JSON safe integer.
TEST(BusinessArguments, wide_integers_and_exact_decimals_never_pass_through_binary64) {
    ayni_schema_node_v1 schema{};
    schema.kind = AYNI_VALUE_U64;
    schema.subtree_end = 1;
    schema.max_u64 = std::numeric_limits<uint64_t>::max();
    const ayni_schema_v1 definition{&schema, 1};
    ayni_argument_node_v1 source{};
    source.kind = AYNI_ARGUMENT_STRING;
    source.subtree_end = 1;
    source.value.text = spelling("18446744073709551615");
    const ayni_arguments_v1 input{&source, 1};
    Output output;
    ASSERT_EQ(ayni_arguments_normalize(&definition, &input, &output.view), AYNI_OK);
    EXPECT_EQ(output.nodes[0].value.unsigned_integer, std::numeric_limits<uint64_t>::max());
    source.kind = AYNI_ARGUMENT_NUMBER;
    EXPECT_EQ(ayni_arguments_normalize(&definition, &input, &output.view), AYNI_INVALID_ARGUMENT);
    schema.kind = AYNI_VALUE_DECIMAL;
    schema.scale = 2;
    schema.min_i64 = -500;
    schema.max_i64 = 500;
    source.kind = AYNI_ARGUMENT_STRING;
    source.value.text = spelling("1.2300");
    ASSERT_EQ(ayni_arguments_normalize(&definition, &input, &output.view), AYNI_OK);
    EXPECT_EQ(output.nodes[0].value.decimal.coefficient, 123);
    source.value.text = spelling("1.231");
    EXPECT_EQ(ayni_arguments_normalize(&definition, &input, &output.view), AYNI_INVALID_ARGUMENT);
    source.value.text = spelling("1e0");
    EXPECT_EQ(ayni_arguments_normalize(&definition, &input, &output.view), AYNI_INVALID_ARGUMENT);
}

// ADR-0084 approximate-number representation: finite subnormals survive; an
// actual nonzero underflow, overflow, invalid grammar and NaN are refused.
TEST(BusinessArguments, approximate_numbers_round_once_and_keep_legal_subnormals) {
    ayni_schema_node_v1 schema{};
    schema.kind = AYNI_VALUE_FLOAT64;
    schema.subtree_end = 1;
    schema.min_float64 = -std::numeric_limits<double>::max();
    schema.max_float64 = std::numeric_limits<double>::max();
    const ayni_schema_v1 definition{&schema, 1};
    ayni_argument_node_v1 source{};
    source.kind = AYNI_ARGUMENT_NUMBER;
    source.subtree_end = 1;
    source.value.text = spelling("5e-324");
    const ayni_arguments_v1 input{&source, 1};
    Output output;
    ASSERT_EQ(ayni_arguments_normalize(&definition, &input, &output.view), AYNI_OK);
    EXPECT_EQ(output.nodes[0].value.approximate, std::numeric_limits<double>::denorm_min());
    source.value.text = spelling("-0.0");
    ASSERT_EQ(ayni_arguments_normalize(&definition, &input, &output.view), AYNI_OK);
    EXPECT_FALSE(std::signbit(output.nodes[0].value.approximate));
    for (const char *invalid : {"1e-400", "1e400", "NaN", "+1", "01", "1.", "1e"}) {
        SCOPED_TRACE(invalid);
        source.value.text = spelling(invalid);
        output.view.node_count = 99;
        EXPECT_EQ(ayni_arguments_normalize(&definition, &input, &output.view),
                  AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(output.view.node_count, 99U);
    }
}

// ADR-0084 closed objects: names resolve to stable IDs, duplicate/unknown keys
// fail atomically, and defaults are supplied by the shared native planner.
TEST(BusinessArguments, named_fields_share_native_defaults_and_reject_duplicate_keys) {
    std::array<ayni_schema_node_v1, 3> schema{};
    schema[0].kind = AYNI_VALUE_OBJECT;
    schema[0].subtree_end = 3;
    schema[1].kind = AYNI_VALUE_I64;
    schema[1].field_id = 20;
    schema[1].subtree_end = 2;
    schema[1].flags = AYNI_SCHEMA_REQUIRED;
    schema[1].name = spelling("count");
    schema[1].max_i64 = 100;
    schema[2].kind = AYNI_VALUE_TEXT;
    schema[2].field_id = 10;
    schema[2].subtree_end = 3;
    schema[2].name = spelling("label");
    schema[2].max_count = 16;
    ayni_value_node_v1 default_node{};
    default_node.kind = AYNI_VALUE_TEXT;
    default_node.subtree_end = 1;
    default_node.value.text = spelling("original");
    const ayni_value_v1 default_value{&default_node, 1};
    schema[2].default_value = &default_value;
    const ayni_schema_v1 definition{schema.data(), static_cast<uint32_t>(schema.size())};
    std::array<ayni_argument_node_v1, 3> source{};
    source[0].kind = AYNI_ARGUMENT_OBJECT;
    source[0].subtree_end = 2;
    source[1].kind = AYNI_ARGUMENT_NUMBER;
    source[1].subtree_end = 2;
    source[1].name = spelling("count");
    source[1].value.text = spelling("7");
    ayni_arguments_v1 input{source.data(), 2};
    Output output;
    ASSERT_EQ(ayni_arguments_normalize(&definition, &input, &output.view), AYNI_OK);
    ASSERT_EQ(output.view.node_count, 3U);
    EXPECT_EQ(output.nodes[1].field_id, 10U);
    EXPECT_EQ(output.nodes[2].field_id, 20U);
    EXPECT_EQ(output.nodes[2].value.integer, 7);
    ayni::ValueBuilder<2> builder;
    ASSERT_EQ(builder.object(), AYNI_OK);
    ASSERT_EQ(builder.integer(20, 7), AYNI_OK);
    ASSERT_EQ(builder.end(), AYNI_OK);
    ayni_value_v1 native{};
    ASSERT_EQ(builder.finish(native), AYNI_OK);
    Output native_output;
    ASSERT_EQ(ayni_value_normalize(&definition, &native, &native_output.view), AYNI_OK);
    const ayni_value_v1 named_value{output.nodes.data(), output.view.node_count};
    const ayni_value_v1 native_value{native_output.nodes.data(), native_output.view.node_count};
    std::array<uint8_t, 256> named_bytes{}, native_bytes{};
    uint32_t named_size = 0, native_size = 0;
    ASSERT_EQ(ayni_value_write(&named_value, named_bytes.data(),
                               static_cast<uint32_t>(named_bytes.size()), &named_size),
              AYNI_OK);
    ASSERT_EQ(ayni_value_write(&native_value, native_bytes.data(),
                               static_cast<uint32_t>(native_bytes.size()), &native_size),
              AYNI_OK);
    ASSERT_EQ(named_size, native_size);
    EXPECT_EQ(named_bytes, native_bytes);
    source[0].subtree_end = 3;
    source[2] = source[1];
    source[2].subtree_end = 3;
    input.node_count = 3;
    output.view.node_count = 99;
    EXPECT_EQ(ayni_arguments_normalize(&definition, &input, &output.view), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(output.view.node_count, 99U);
    source[0].subtree_end = 2;
    input.node_count = 2;
    source[1].name = spelling("unknown");
    EXPECT_EQ(ayni_arguments_normalize(&definition, &input, &output.view), AYNI_INVALID_ARGUMENT);
    source[1].name = spelling("count");
    source[1].kind = AYNI_ARGUMENT_BOOL;
    source[1].value.boolean = 1;
    EXPECT_EQ(ayni_arguments_normalize(&definition, &input, &output.view), AYNI_INVALID_ARGUMENT);
}
} // namespace

// ADR-0084: a mailbox can retain exact number spellings without retaining a JSON
// DOM or guessing the business type. The portable body carries no native padding.
TEST(BusinessArguments, portable_named_body_retains_numeric_lexemes_and_is_atomic) {
    std::array<ayni_argument_node_v1, 2> source{};
    source[0].kind = AYNI_ARGUMENT_OBJECT;
    source[0].subtree_end = 2;
    source[1].kind = AYNI_ARGUMENT_NUMBER;
    source[1].subtree_end = 2;
    source[1].name = spelling("measurement");
    source[1].value.text = spelling("0.1000000000000000000000001");
    const ayni_arguments_v1 input{source.data(), static_cast<uint32_t>(source.size())};
    std::array<uint8_t, 256> body{};
    body.fill(91);
    uint32_t size = 99;
    EXPECT_EQ(ayni::catalog::encode_arguments(input, body.data(), 1, size), AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(body[0], 91U);
    ASSERT_EQ(ayni::catalog::encode_arguments(input, body.data(),
                                              static_cast<uint32_t>(body.size()), size),
              AYNI_OK);
    ayni::catalog::DecodedArguments decoded;
    ASSERT_EQ(ayni::catalog::decode_arguments({body.data(), size}, decoded), AYNI_OK);
    EXPECT_EQ(decoded.nodes[1].kind, AYNI_ARGUMENT_NUMBER);
    EXPECT_EQ(std::string(decoded.nodes[1].value.text.data, decoded.nodes[1].value.text.size),
              "0.1000000000000000000000001");
    for (uint32_t end = 0; end < size; ++end) {
        decoded.count = 99;
        EXPECT_NE(ayni::catalog::decode_arguments({body.data(), end}, decoded), AYNI_OK) << end;
        EXPECT_EQ(decoded.count, 99U);
    }
}

// SDK construction refuses incomplete or exhausted trees without publishing a
// partial view; successful output is admitted by the same public native validator.
TEST(BusinessValues, cpp_builder_publishes_complete_trees_and_keeps_exhaustion_explicit) {
    ayni::ValueBuilder<3> builder;
    ASSERT_EQ(builder.object(), AYNI_OK);
    ASSERT_EQ(builder.list(1), AYNI_OK);
    ASSERT_EQ(builder.decimal(0, {123, 2, 0}), AYNI_OK);
    ayni_value_v1 out{};
    EXPECT_EQ(builder.finish(out), AYNI_INVALID_STATE);
    EXPECT_EQ(out.node_count, 0U);
    ASSERT_EQ(builder.end(), AYNI_OK);
    ASSERT_EQ(builder.end(), AYNI_OK);
    ASSERT_EQ(builder.finish(out), AYNI_OK);
    EXPECT_EQ(ayni_value_validate(&out), AYNI_OK);
    builder.clear();
    ASSERT_EQ(builder.list(), AYNI_OK);
    ASSERT_EQ(builder.integer(0, 1), AYNI_OK);
    ASSERT_EQ(builder.integer(0, 2), AYNI_OK);
    EXPECT_EQ(builder.integer(0, 3), AYNI_CAPACITY_EXCEEDED);
    out.node_count = 99;
    EXPECT_EQ(builder.finish(out), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(out.node_count, 99U);
}

// ADR-0084: the SDK edge printer keeps signed coefficient extremes and fixed
// scale exact; a small output lane never reports a complete truncated JSON value.
TEST(BusinessValues, json_sdk_keeps_decimal_extremes_and_explicit_capacity_failure) {
    struct Case {
        int64_t coefficient;
        uint32_t scale;
        const char *expected;
    };
    const Case cases[] = {{INT64_MIN, 18, "\"-9.223372036854775808\""},
                          {INT64_MIN, 0, "\"-9223372036854775808\""},
                          {1, 18, "\"0.000000000000000001\""},
                          {0, 2, "\"0.00\""}};
    for (const auto &test : cases) {
        ayni_schema_node_v1 node{};
        node.kind = AYNI_VALUE_DECIMAL;
        node.subtree_end = 1;
        node.min_i64 = INT64_MIN;
        node.max_i64 = INT64_MAX;
        node.scale = test.scale;
        const ayni_schema_v1 schema{&node, 1};
        ayni::ValueBuilder<1> builder;
        builder.decimal(0, {test.coefficient, test.scale, 0});
        ayni_value_v1 input{};
        ASSERT_EQ(builder.finish(input), AYNI_OK);
        ayni_value_node_v1 normalized_node{};
        ayni_value_buffer_v1 normalized{&normalized_node, 1, 0, nullptr, 0, 0};
        ASSERT_EQ(ayni_value_normalize(&schema, &input, &normalized), AYNI_OK);
        const ayni_value_v1 value{&normalized_node, 1};
        std::array<char, 64> bytes{};
        ayni::json::Writer output(bytes.data(), bytes.size());
        ASSERT_EQ(ayni::json::write_value(output, schema, value), AYNI_OK);
        size_t size = 0;
        ASSERT_TRUE(output.finish(size));
        EXPECT_EQ(std::string(bytes.data(), size), test.expected);
        bytes.fill('x');
        ayni::json::Writer small(bytes.data(), 2);
        ASSERT_EQ(ayni::json::write_value(small, schema, value), AYNI_OK);
        EXPECT_FALSE(small.finish(size));
        EXPECT_EQ(bytes[2], 'x');
    }
}

// ADR-0084: unrelated Schema members are ignored by the shared validator and
// by the public JSON facade; no choices pointer is read for another kind.
TEST(BusinessValues, json_schema_ignores_unrelated_choice_metadata) {
    const ayni_value_kind_t kinds[] = {AYNI_VALUE_I64, AYNI_VALUE_BOOL, AYNI_VALUE_TEXT};
    for (const auto kind : kinds) {
        ayni_schema_node_v1 node{};
        node.kind = kind;
        node.subtree_end = 1;
        node.max_i64 = 10;
        node.max_count = 20;
        node.choice_count = UINT32_MAX;
        node.choices = nullptr;
        const ayni_schema_v1 schema{&node, 1};
        ASSERT_EQ(ayni_schema_validate(&schema), AYNI_OK);
        std::array<char, 256> bytes{};
        ayni::json::Writer output(bytes.data(), bytes.size());
        ayni::json::write_schema(output, schema);
        size_t size = 0;
        ASSERT_TRUE(output.finish(size));
        const std::string json(bytes.data(), size);
        EXPECT_EQ(json.find("enum"), std::string::npos);
        EXPECT_NE(json.find(kind == AYNI_VALUE_I64    ? "integer"
                            : kind == AYNI_VALUE_BOOL ? "boolean"
                                                      : "string"),
                  std::string::npos);
    }
}
