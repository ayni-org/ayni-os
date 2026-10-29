// Explicit business contract for the deterministic mock display fixtures.
#ifndef AYNI_TEST_MOCK_DISPLAY_SCHEMA_H
#define AYNI_TEST_MOCK_DISPLAY_SCHEMA_H
#include "work_node.h"
namespace ayni::test {
struct MockDisplaySchema {
    static constexpr uint32_t body_field = UINT32_MAX;
    ayni_schema_node_v1 input[8]{};
    ayni_schema_node_v1 result{};
    ayni_authority_projection_v1 projections[2]{};
    ayni_business_contract_v1 business{};
    MockDisplaySchema() {
        static const ayni_string_v1 voices[] = {text("bright"), text("calm")};
        input[0].kind = AYNI_VALUE_OBJECT;
        input[0].subtree_end = 4;
        auto &seconds = input[1];
        seconds.kind = AYNI_VALUE_I64;
        seconds.field_id = 1;
        seconds.subtree_end = 2;
        seconds.flags = AYNI_SCHEMA_REQUIRED;
        seconds.name = text("seconds");
        seconds.unit = text("second");
        seconds.min_i64 = 1;
        seconds.max_i64 = 60;
        auto &voice = input[2];
        voice.kind = AYNI_VALUE_ENUM;
        voice.field_id = 2;
        voice.subtree_end = 3;
        voice.flags = AYNI_SCHEMA_REQUIRED;
        voice.name = text("voice");
        voice.choice_count = 2;
        voice.choices = voices;
        auto &body = input[3];
        body.kind = AYNI_VALUE_TEXT;
        body.field_id = body_field;
        body.subtree_end = 4;
        body.flags = AYNI_SCHEMA_REQUIRED;
        body.name = text("text");
        body.max_count = 64;
        result.kind = AYNI_VALUE_TEXT;
        result.subtree_end = 1;
        result.max_count = 64;
        projections[0].field_id = 1;
        projections[0].path_count = 1;
        projections[0].path[0] = 1;
        projections[0].unit = text("second");
        projections[1].field_id = 2;
        projections[1].path_count = 1;
        projections[1].path[0] = 2;
        business = {{input, 4}, {&result, 1}, 2, 0, projections};
    }
};
} // namespace ayni::test
#endif
