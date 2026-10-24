// White-box static call declaration tests, C-CALL-001/C-BIND-001, ADR-0045.
// Catalog compatibility is not permission, Provider dispatch or resource admission.
#include "catalog/capability/call_contract.h"

#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

namespace {
using namespace ayni::catalog;

CatalogId id(const char *text) {
    CatalogId value;
    EXPECT_TRUE(value.assign({text, static_cast<uint32_t>(std::strlen(text))}));
    return value;
}

CallContract contract() {
    CallContract value{};
    value.form = CallForm::unary;
    value.category = CallClass::model;
    value.effects = call_effect_egress | call_effect_billing;
    value.idempotency = CallIdempotency::not_idempotent;
    value.query = QueryAuthority::none;
    value.cancel = CancelMode::request;
    value.timeout_ms = 30000;
    value.input_format = call_fields_text_input_format;
    value.result_format = call_text_result_format;
    value.input_text_limit = 4096;
    value.result_text_limit = 2048;
    return value;
}

TEST(CallContract, canonical_resources_have_one_order_and_keep_required_units) {
    auto input = contract();
    input.resource_count = 2;
    input.resources[0] = {id("network"), 2};
    input.resources[1] = {id("model"), 1};
    input.resources[2] = {id("unused"), 99};
    CallContract canonical{};
    ASSERT_EQ(normalize_call_contract(input, canonical), AYNI_OK);
    EXPECT_TRUE(canonical.resources[0].id.equals({"model", 5}));
    EXPECT_TRUE(canonical.resources[1].id.equals({"network", 7}));
    EXPECT_EQ(canonical.resources[0].units, 1U);
    EXPECT_EQ(canonical.resources[1].units, 2U);
    EXPECT_EQ(canonical.resources[2].id.size(), 0U);
    const auto tmp = input.resources[0];
    input.resources[0] = input.resources[1];
    input.resources[1] = tmp;
    CallContract equivalent{};
    ASSERT_EQ(normalize_call_contract(input, equivalent), AYNI_OK);
    EXPECT_TRUE(same_call_contract(canonical, equivalent));
}

TEST(CallContract, duplicate_resources_or_invalid_units_preserve_the_output) {
    auto input = contract();
    input.resource_count = 2;
    input.resources[0] = {id("model"), 1};
    input.resources[1] = {id("model"), 1};
    auto output = contract();
    EXPECT_EQ(normalize_call_contract(input, output), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(output.resource_count, 0U);
    input.resource_count = 1;
    input.resources[0].units = 0;
    EXPECT_EQ(normalize_call_contract(input, output), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(output.resource_count, 0U);
}

TEST(CallContract, each_semantic_change_prevents_exact_provider_compatibility) {
    const auto original = contract();
    using Change = void (*)(CallContract &);
    const Change changes[] = {
        [](auto &v) { v.form = CallForm::action; },
        [](auto &v) { v.category = CallClass::tool; },
        [](auto &v) { v.effects = 0; },
        [](auto &v) { v.idempotency = CallIdempotency::idempotent; },
        [](auto &v) { v.query = QueryAuthority::authoritative; },
        [](auto &v) { v.cancel = CancelMode::confirmed; },
        [](auto &v) { v.dedupe_window_ms = 1; },
        [](auto &v) { ++v.timeout_ms; },
        [](auto &v) { ++v.input_format; },
        [](auto &v) { ++v.result_format; },
        [](auto &v) { ++v.input_text_limit; },
        [](auto &v) { ++v.result_text_limit; },
        [](auto &v) {
            v.resource_count = 1;
            v.resources[0] = {id("model"), 1};
        },
    };
    for (const auto change : changes) {
        auto changed = original;
        change(changed);
        EXPECT_FALSE(same_call_contract(original, changed));
    }
}

TEST(CallContract, invalid_declarations_and_unknown_formats_do_not_silently_default) {
    auto input = contract();
    CallContract output{};
    input.timeout_ms = 0;
    EXPECT_EQ(normalize_call_contract(input, output), AYNI_INVALID_ARGUMENT);
    input = contract();
    input.effects = call_effect_mask + 1U;
    EXPECT_EQ(normalize_call_contract(input, output), AYNI_INVALID_ARGUMENT);
    input = contract();
    input.input_format = 0;
    EXPECT_EQ(normalize_call_contract(input, output), AYNI_UNSUPPORTED_VERSION);
    input = contract();
    input.result_text_limit = call_text_limit_max + 1U;
    EXPECT_EQ(normalize_call_contract(input, output), AYNI_INVALID_ARGUMENT);
    input = contract();
    input.category = static_cast<CallClass>(0);
    EXPECT_EQ(normalize_call_contract(input, output), AYNI_INVALID_ARGUMENT);
}

TEST(CallContract, side_effects_and_retry_properties_are_independent_declarations) {
    auto input = contract();
    input.form = CallForm::action;
    input.effects = call_effect_physical;
    input.idempotency = CallIdempotency::idempotent;
    input.query = QueryAuthority::best_effort;
    input.dedupe_window_ms = 0;
    CallContract output{};
    ASSERT_EQ(normalize_call_contract(input, output), AYNI_OK);
    EXPECT_EQ(output.effects, call_effect_physical);
    EXPECT_EQ(output.query, QueryAuthority::best_effort);
    EXPECT_EQ(output.dedupe_window_ms, 0U);
    // Recognizable metadata does not enable physical execution without MA-018B.
}

TEST(CallContract, registry_copies_exact_versions_and_refuses_duplicates_or_capacity_exhaustion) {
    constexpr uint32_t slots = 2;
    std::vector<uint64_t> storage(CallCapabilityRegistry::storage_size(slots) / sizeof(uint64_t) +
                                  1U);
    CallCapabilityRegistry registry(storage.data(), slots);
    CallCapability input{id("model.chat"), 1, contract()};
    ASSERT_EQ(registry.add(input), AYNI_OK);
    EXPECT_EQ(registry.add(input), AYNI_INVALID_ARGUMENT);
    input.version = 2;
    input.contract.result_text_limit = 1;
    ASSERT_EQ(registry.add(input), AYNI_OK);
    input.version = 3;
    EXPECT_EQ(registry.add(input), AYNI_CAPACITY_EXCEEDED);
    uint32_t index = 99;
    EXPECT_TRUE(registry.find({"model.chat", 10}, 1, index));
    EXPECT_EQ(registry.at(index).contract.result_text_limit, 2048U);
    EXPECT_TRUE(registry.find({"model.chat", 10}, 2, index));
    EXPECT_EQ(registry.at(index).contract.result_text_limit, 1U);
    EXPECT_FALSE(registry.find({"model.chat", 10}, 3, index));
    EXPECT_EQ(index, 1U);
}

TEST(CallContract, disabled_registry_accepts_no_entries) {
    CallCapabilityRegistry registry(nullptr, 0);
    EXPECT_EQ(registry.add({id("model.chat"), 1, contract()}), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(registry.size(), 0U);
}
} // namespace
