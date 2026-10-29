#include "authority/scope/scope.h"
#include "catalog/capability/authority_projection.h"
#include <gtest/gtest.h>

#include <array>

namespace {
// ADR-0084: one list axis projects exact coefficients and the same fixed
// route on every row. Projection supplies operands; it never chooses a Grant.
TEST(BusinessProjection, exact_decimal_rows_keep_one_route_and_copy_token_storage_safely) {
    std::array<ayni_schema_node_v1, 5> schema{};
    schema[0].kind = AYNI_VALUE_OBJECT;
    schema[0].subtree_end = 5;
    schema[1].kind = AYNI_VALUE_LIST;
    schema[1].field_id = 10;
    schema[1].subtree_end = 4;
    schema[1].flags = AYNI_SCHEMA_REQUIRED;
    schema[1].name = {"items", 5};
    schema[1].min_count = 1;
    schema[1].max_count = 3;
    schema[2].kind = AYNI_VALUE_OBJECT;
    schema[2].subtree_end = 4;
    schema[3].kind = AYNI_VALUE_DECIMAL;
    schema[3].field_id = 20;
    schema[3].subtree_end = 4;
    schema[3].flags = AYNI_SCHEMA_REQUIRED;
    schema[3].name = {"price", 5};
    schema[3].unit = {"yuan", 4};
    schema[3].max_i64 = 10000;
    schema[3].scale = 2;
    const ayni_string_v1 target{"target.a", 8};
    schema[4].kind = AYNI_VALUE_IDENTIFIER;
    schema[4].field_id = 30;
    schema[4].subtree_end = 5;
    schema[4].flags = AYNI_SCHEMA_REQUIRED;
    schema[4].name = {"target", 6};
    schema[4].choice_count = 1;
    schema[4].choices = &target;
    ayni_schema_node_v1 result{};
    result.kind = AYNI_VALUE_TEXT;
    result.subtree_end = 1;
    result.max_count = 64;
    std::array<ayni_authority_projection_v1, 2> paths{};
    paths[0].field_id = 1;
    paths[0].path_count = 3;
    paths[0].path[0] = 10;
    paths[0].path[1] = AYNI_PROJECTION_LIST_ITEM;
    paths[0].path[2] = 20;
    paths[0].scale = 2;
    paths[0].unit = {"cent", 4};
    paths[1].field_id = 2;
    paths[1].path_count = 1;
    paths[1].path[0] = 30;
    const ayni_business_contract_v1 business{
        {schema.data(), static_cast<uint32_t>(schema.size())}, {&result, 1}, 2, 0, paths.data()};
    std::array<ayni_authority_field_desc_v1, 2> fields{};
    for (uint32_t at = 0; at < fields.size(); ++at) {
        fields[at].struct_size = sizeof(fields[at]);
        fields[at].field_id = at + 1U;
        fields[at].domain.struct_size = sizeof(fields[at].domain);
    }
    fields[0].usage = AYNI_AUTHORITY_PARAMETER;
    fields[0].domain.kind = AYNI_AUTHORITY_I64;
    fields[0].domain.unit = {"cent", 4};
    fields[0].domain.values.integer = {0, 10000};
    fields[1].usage = AYNI_AUTHORITY_EXECUTION_TARGET;
    fields[1].domain.kind = AYNI_AUTHORITY_IDENTIFIER;
    fields[1].domain.values.tokens = {&target, 1, 0};
    std::array<uint8_t, 512> domain{};
    uint32_t domain_size = 0;
    ASSERT_EQ(ayni::catalog::encode_domain(fields.data(), 2, domain.data(),
                                           static_cast<uint32_t>(domain.size()), domain_size),
              AYNI_OK);
    ayni::catalog::ProjectionShape shape;
    ASSERT_EQ(ayni::catalog::validate_projection({domain.data(), domain_size}, business, shape),
              AYNI_OK);
    ASSERT_TRUE(shape.list_node);
    EXPECT_EQ(*shape.list_node, 1U);
    std::array<ayni_value_node_v1, 7> nodes{};
    nodes[0].kind = AYNI_VALUE_OBJECT;
    nodes[0].subtree_end = 7;
    nodes[1].kind = AYNI_VALUE_LIST;
    nodes[1].field_id = 10;
    nodes[1].subtree_end = 6;
    nodes[2].kind = AYNI_VALUE_OBJECT;
    nodes[2].subtree_end = 4;
    nodes[3].kind = AYNI_VALUE_DECIMAL;
    nodes[3].field_id = 20;
    nodes[3].subtree_end = 4;
    nodes[3].value.decimal = {120, 2, 0};
    nodes[4].kind = AYNI_VALUE_OBJECT;
    nodes[4].subtree_end = 6;
    nodes[5].kind = AYNI_VALUE_DECIMAL;
    nodes[5].field_id = 20;
    nodes[5].subtree_end = 6;
    nodes[5].value.decimal = {240, 2, 0};
    nodes[6].kind = AYNI_VALUE_IDENTIFIER;
    nodes[6].field_id = 30;
    nodes[6].subtree_end = 7;
    nodes[6].value.text = target;
    const ayni_value_v1 value{nodes.data(), static_cast<uint32_t>(nodes.size())};
    ASSERT_EQ(ayni::catalog::projection_rows(business, value), 2U);
    ayni::catalog::ProjectionRow first, second;
    ASSERT_EQ(ayni::catalog::project_row(business, value, 0, first), AYNI_OK);
    ASSERT_EQ(ayni::catalog::project_row(business, value, 1, second), AYNI_OK);
    EXPECT_EQ(first.fields[0].allowed.values.integer.min_inclusive, 120);
    EXPECT_EQ(second.fields[0].allowed.values.integer.min_inclusive, 240);
    const auto copied = first;
    EXPECT_EQ(copied.fields[1].allowed.values.tokens.members, &copied.tokens[1]);
    EXPECT_NE(copied.fields[1].allowed.values.tokens.members,
              first.fields[1].allowed.values.tokens.members);
    EXPECT_EQ(ayni::catalog::project_row(business, value, 2, second), AYNI_INVALID_ARGUMENT);
    schema[1].min_count = 0;
    EXPECT_EQ(ayni::catalog::validate_projection({domain.data(), domain_size}, business, shape),
              AYNI_UNSUPPORTED);
    schema[1].min_count = 1;
    // ADR-0084: scale zero carries the same unit, with no implicit conversion.
    schema[3].scale = 0;
    paths[0].scale = 0;
    EXPECT_EQ(ayni::catalog::validate_projection({domain.data(), domain_size}, business, shape),
              AYNI_INVALID_ARGUMENT);
    schema[3].unit = paths[0].unit;
    EXPECT_EQ(ayni::catalog::validate_projection({domain.data(), domain_size}, business, shape),
              AYNI_OK);
    schema[3].scale = 2;
    paths[0].scale = 2;
    schema[3].unit = {"yuan", 4};
    schema[3].flags |= AYNI_SCHEMA_NULLABLE;
    EXPECT_EQ(ayni::catalog::validate_projection({domain.data(), domain_size}, business, shape),
              AYNI_UNSUPPORTED);
}
} // namespace
