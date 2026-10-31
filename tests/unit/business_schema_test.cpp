#include "catalog/capability/business_encoding.h"
#include "catalog/capability/capability_registry.h"
#include "catalog/capability/schema_encoding.h"
#include "workspace_fixture.h"
#include <gtest/gtest.h>

#include <array>
#include <cstring>

namespace {
// ADR-0084: original defaults, names, bounds and exact kind distinctions are
// canonical machine proof; changing only a scale must change that proof.
TEST(BusinessSchema, canonical_proof_sorts_declarations_and_retains_exact_decimal_scale) {
    std::array<ayni_schema_node_v1, 3> nodes{};
    nodes[0].kind = AYNI_VALUE_OBJECT;
    nodes[0].subtree_end = static_cast<uint32_t>(nodes.size());
    nodes[1].kind = AYNI_VALUE_DECIMAL;
    nodes[1].field_id = 2;
    nodes[1].subtree_end = 2;
    nodes[1].flags = AYNI_SCHEMA_REQUIRED;
    nodes[1].name = {"price", 5};
    nodes[1].unit = {"cent", 4};
    nodes[1].max_i64 = 9999;
    nodes[1].scale = 2;
    nodes[2].kind = AYNI_VALUE_TEXT;
    nodes[2].field_id = 1;
    nodes[2].subtree_end = 3;
    nodes[2].name = {"label", 5};
    nodes[2].max_count = 16;
    ayni_value_node_v1 default_node{};
    default_node.kind = AYNI_VALUE_TEXT;
    default_node.subtree_end = 1;
    default_node.value.text = {"default", 7};
    const ayni_value_v1 default_value{&default_node, 1};
    nodes[2].default_value = &default_value;
    ayni_schema_v1 schema{nodes.data(), static_cast<uint32_t>(nodes.size())};
    std::array<uint8_t, 1024> bytes{};
    uint32_t size = 0;
    ASSERT_EQ(ayni::catalog::encode_schema(::ayni::test::codec_workspace(), schema, bytes.data(),
                                           static_cast<uint32_t>(bytes.size()), size),
              AYNI_OK);
    auto reordered = nodes;
    reordered[1] = nodes[2];
    reordered[1].subtree_end = 2;
    reordered[2] = nodes[1];
    reordered[2].subtree_end = 3;
    const ayni_schema_v1 same{reordered.data(), static_cast<uint32_t>(reordered.size())};
    std::array<uint8_t, 1024> other{};
    uint32_t other_size = 0;
    ASSERT_EQ(ayni::catalog::encode_schema(::ayni::test::codec_workspace(), same, other.data(),
                                           static_cast<uint32_t>(other.size()), other_size),
              AYNI_OK);
    ASSERT_EQ(size, other_size);
    EXPECT_EQ(std::memcmp(bytes.data(), other.data(), size), 0);
    ayni::catalog::DecodedSchema decoded;
    ASSERT_EQ(ayni::catalog::decode_schema(::ayni::test::codec_workspace(), {bytes.data(), size},
                                           decoded),
              AYNI_OK);
    ASSERT_EQ(decoded.count, 3U);
    EXPECT_EQ(decoded.nodes[1].field_id, 1U);
    ASSERT_NE(decoded.nodes[1].default_value, nullptr);
    EXPECT_EQ(decoded.nodes[2].scale, 2U);
    const auto copy = decoded;
    EXPECT_NE(copy.nodes[1].default_value, decoded.nodes[1].default_value);
    EXPECT_NE(copy.nodes[1].default_value->nodes, decoded.nodes[1].default_value->nodes);
    EXPECT_EQ(copy.nodes[1].default_value->nodes[0].value.text.size, 7U);
    nodes[1].scale = 3;
    ASSERT_EQ(ayni::catalog::encode_schema(::ayni::test::codec_workspace(), schema, other.data(),
                                           static_cast<uint32_t>(other.size()), other_size),
              AYNI_OK);
    EXPECT_NE(std::memcmp(bytes.data(), other.data(), size), 0);
}

// ADR-0084: encoded input is a complete canonical body; ordinary malformed
// API bytes cannot publish half a schema or corrupt the prior borrowed view.
TEST(BusinessSchema, truncated_proof_and_small_output_preserve_previous_storage) {
    ayni_schema_node_v1 node{};
    node.kind = AYNI_VALUE_I64;
    node.subtree_end = 1;
    node.min_i64 = -7;
    node.max_i64 = 12;
    const ayni_schema_v1 schema{&node, 1};
    std::array<uint8_t, 128> bytes{};
    bytes.fill(91);
    uint32_t size = 19;
    EXPECT_EQ(ayni::catalog::encode_schema(::ayni::test::codec_workspace(), schema, bytes.data(), 1,
                                           size),
              AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(bytes[0], 91U);
    ASSERT_EQ(ayni::catalog::encode_schema(::ayni::test::codec_workspace(), schema, bytes.data(),
                                           static_cast<uint32_t>(bytes.size()), size),
              AYNI_OK);
    ayni::catalog::DecodedSchema decoded;
    for (uint32_t length = 0; length < size; ++length) {
        SCOPED_TRACE(length);
        decoded.count = 91;
        EXPECT_EQ(ayni::catalog::decode_schema(::ayni::test::codec_workspace(),
                                               {bytes.data(), length}, decoded),
                  AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(decoded.count, 91U);
    }
    ASSERT_EQ(ayni::catalog::decode_schema(::ayni::test::codec_workspace(), {bytes.data(), size},
                                           decoded),
              AYNI_OK);
    EXPECT_EQ(decoded.nodes[0].min_i64, -7);
    EXPECT_EQ(decoded.nodes[0].max_i64, 12);
}
} // namespace

// ADR-0084: the complete proof includes a projection count even for an empty
// authority domain; nonempty mappings survive exact registration and decode.
TEST(BusinessContract, complete_proof_round_trips_empty_and_nonempty_projection_sets) {
    for (const uint32_t count : {0U, 1U}) {
        SCOPED_TRACE(count);
        std::array<ayni_schema_node_v1, 2> input{};
        input[0].kind = AYNI_VALUE_OBJECT;
        input[0].subtree_end = 2;
        input[1].kind = AYNI_VALUE_I64;
        input[1].field_id = 7;
        input[1].subtree_end = 2;
        input[1].flags = AYNI_SCHEMA_REQUIRED;
        input[1].name = {"count", 5};
        input[1].unit = {"item", 4};
        input[1].max_i64 = 100;
        ayni_schema_node_v1 result{};
        result.kind = AYNI_VALUE_TEXT;
        result.subtree_end = 1;
        result.max_count = 16;
        ayni_authority_projection_v1 path{};
        path.field_id = 1;
        path.path_count = 1;
        path.path[0] = 7;
        path.unit = {"item", 4};
        const ayni_business_contract_v1 business{
            {input.data(), 2}, {&result, 1}, count, 0, count == 0U ? nullptr : &path};
        ayni_authority_field_desc_v1 field{};
        field.struct_size = sizeof(field);
        field.field_id = 1;
        field.usage = AYNI_AUTHORITY_PARAMETER;
        field.domain.struct_size = sizeof(field.domain);
        field.domain.kind = AYNI_AUTHORITY_I64;
        field.domain.unit = path.unit;
        field.domain.values.integer = {0, 100};
        std::array<uint8_t, 128> domain{};
        uint32_t domain_size = 0;
        ASSERT_EQ(ayni::catalog::encode_domain(count == 0U ? nullptr : &field, count, domain.data(),
                                               static_cast<uint32_t>(domain.size()), domain_size),
                  AYNI_OK);
        ayni::catalog::CallContract execution{};
        execution.form = ayni::catalog::CallForm::unary;
        execution.category = ayni::catalog::CallClass::tool;
        execution.idempotency = ayni::catalog::CallIdempotency::idempotent;
        execution.query = ayni::catalog::QueryAuthority::none;
        execution.cancel = ayni::catalog::CancelMode::none;
        execution.timeout_ms = 1000;
        execution.input_format = AYNI_CALL_INPUT_VALUE;
        execution.result_format = AYNI_CALL_RESULT_VALUE;
        execution.input_text_limit = 512;
        execution.result_text_limit = 512;
        std::array<uint8_t, 4096> bytes{};
        uint32_t size = 0;
        ASSERT_EQ(ayni::catalog::encode_business(
                      ::ayni::test::codec_workspace(), {domain.data(), domain_size}, execution,
                      business, bytes.data(), static_cast<uint32_t>(bytes.size()), size),
                  AYNI_OK);
        ayni::catalog::DecodedBusiness decoded;
        ASSERT_EQ(ayni::catalog::decode_business(::ayni::test::codec_workspace(),
                                                 {domain.data(), domain_size}, {bytes.data(), size},
                                                 decoded),
                  AYNI_OK);
        EXPECT_EQ(decoded.count, count);
        EXPECT_EQ(decoded.input.nodes[1].field_id, 7U);
        execution.business_contract = {bytes.data(), size};
        EXPECT_TRUE(ayni::catalog::same_call_contract(decoded.execution, execution));
        if (count != 0U) {
            EXPECT_EQ(decoded.projections[0].path[0], 7U);
        }
        for (uint32_t end = 0; end < size; ++end) {
            ayni::catalog::DecodedBusiness refused;
            refused.count = 99;
            EXPECT_NE(ayni::catalog::decode_business(::ayni::test::codec_workspace(),
                                                     {domain.data(), domain_size},
                                                     {bytes.data(), end}, refused),
                      AYNI_OK)
                << end;
            EXPECT_EQ(refused.count, 99U);
        }
    }
}
