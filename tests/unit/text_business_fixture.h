// Explicit TEXT business contracts for protocol-independent call lifecycle fixtures.
#ifndef AYNI_TEST_TEXT_BUSINESS_FIXTURE_H
#define AYNI_TEST_TEXT_BUSINESS_FIXTURE_H
#include "catalog/capability/business_encoding.h"
#include <ayni/values.hpp>
namespace ayni::test {
struct TextBusiness {
    ayni_schema_node_v1 input{}, result{};
    ayni_business_contract_v1 definition{};
    TextBusiness(uint32_t input_limit, uint32_t result_limit) {
        input.kind = result.kind = AYNI_VALUE_TEXT;
        input.subtree_end = result.subtree_end = 1;
        input.max_count = input_limit;
        result.max_count = result_limit;
        definition = {{&input, 1}, {&result, 1}, 0, 0, nullptr};
    }
};
struct TextValue {
    ayni_value_node_v1 node{};
    explicit TextValue(ayni_string_v1 text) {
        node.kind = AYNI_VALUE_TEXT;
        node.subtree_end = 1;
        node.value.text = text;
    }
    ayni_value_v1 view() const { return {&node, 1}; }
};
} // namespace ayni::test
#endif
