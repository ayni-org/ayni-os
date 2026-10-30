// White-box static call declaration tests, C-CALL-001/C-BIND-001, ADR-0045.
// Catalog compatibility is not permission, Provider dispatch or resource admission.
#include "catalog/capability/call_contract.h"
#include "catalog/provider/provider_registry.h"
#include "text_business_fixture.h"

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
    value.input_format = call_value_input_format;
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

// ADR-0087: saved logical proof permits only a bijection of unchanged demand units.
TEST(CallContract, binding_resource_substitution_keeps_semantics_and_unit_multiplicity) {
    auto logical = contract();
    logical.resource_count = 2;
    logical.resources[0] = {id("logical.a"), 1};
    logical.resources[1] = {id("logical.b"), 2};
    auto effective = logical;
    effective.resources[0] = {id("actual.a"), 2};
    effective.resources[1] = {id("actual.b"), 1};
    EXPECT_TRUE(bound_call_contract(logical, effective));
    EXPECT_FALSE(same_call_contract(logical, effective));
    effective.resources[1].units = 2;
    EXPECT_FALSE(bound_call_contract(logical, effective));
    effective = logical;
    --effective.resource_count;
    EXPECT_FALSE(bound_call_contract(logical, effective));
    effective = logical;
    ++effective.timeout_ms;
    EXPECT_FALSE(bound_call_contract(logical, effective));
}

// C-BIND-004 / ADR-0087: static, already admitted contract matching; no query I/O,
// UNKNOWN injection or new recovery campaign. The original proof remains authoritative.
TEST(CallContract, saved_query_matches_logical_proof_and_route_with_actual_resources) {
    auto logical = contract();
    logical.resource_count = 1;
    logical.resources[0] = {id("logical"), 1};
    const std::array<uint8_t, 1> proof{{1}}, changed_proof{{0}};
    logical.business_contract = {proof.data(), static_cast<uint32_t>(proof.size())};
    Provider provider{};
    provider.endpoint = id("endpoint");
    provider.destination = id("cloud");
    provider.count = 1;
    provider.offers[0] = {id("model.chat"), 1, logical};
    auto actual = logical;
    actual.resources[0].id = id("actual");
    const auto matches = [&](const auto &value, ayni_string_v1 endpoint = {"endpoint", 8}) {
        return ProviderRegistry::matches_saved_call(provider, {"model.chat", 10}, 1, value,
                                                    endpoint, {"cloud", 5});
    };
    EXPECT_TRUE(matches(actual));
    EXPECT_FALSE(ProviderRegistry::matches(provider, {"model.chat", 10}, 1, actual, {"endpoint", 8},
                                           {"cloud", 5}));
    EXPECT_FALSE(matches(actual, {"changed", 7}));
    auto changed = actual;
    changed.resources[0].units = 2;
    EXPECT_FALSE(matches(changed));
    changed = actual;
    changed.business_contract = {changed_proof.data(), static_cast<uint32_t>(changed_proof.size())};
    EXPECT_FALSE(matches(changed));
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

// C-CALL-001: enums, bounds and collection contracts are explicit. Refusal
// leaves the previous canonical output intact, including its complete resource group.
TEST(CallContract, invalid_enum_bounds_and_subscription_declarations_preserve_output) {
    const auto sentinel = contract();
    const auto changes = {
        +[](CallContract &v) { v.form = static_cast<CallForm>(0); },
        +[](CallContract &v) { v.idempotency = static_cast<CallIdempotency>(0); },
        +[](CallContract &v) { v.query = static_cast<QueryAuthority>(0); },
        +[](CallContract &v) { v.cancel = static_cast<CancelMode>(0); },
        +[](CallContract &v) { v.input_text_limit = AYNI_CALL_INPUT_BYTES_MAX + 1; },
        +[](CallContract &v) { v.resource_count = resource_group_max + 1; },
        +[](CallContract &v) {
            v.resource_count = 1;
            v.resources[0] = {{}, 1};
        },
        +[](CallContract &v) { v.form = CallForm::subscription; },
    };
    for (const auto change : changes) {
        auto candidate = contract();
        change(candidate);
        auto output = sentinel;
        EXPECT_EQ(normalize_call_contract(candidate, output), AYNI_INVALID_ARGUMENT);
        EXPECT_TRUE(same_call_contract(output, sentinel));
    }
    auto subscription = contract();
    subscription.form = CallForm::subscription;
    subscription.category = CallClass::tool;
    subscription.effects = 0;
    subscription.cancel = CancelMode::confirmed;
    subscription.subscription = SubscriptionContract{id("camera"), 1, 500};
    CallContract normalized{};
    ASSERT_EQ(normalize_call_contract(subscription, normalized), AYNI_OK);
    const auto invalid_collections = {
        +[](CallContract &v) { v.form = CallForm::unary; },
        +[](CallContract &v) { v.category = CallClass::model; },
        +[](CallContract &v) { v.effects = call_effect_physical; },
        +[](CallContract &v) { v.cancel = CancelMode::none; },
        +[](CallContract &v) { v.subscription->source = {}; },
        +[](CallContract &v) { v.subscription->source_version = 0; },
        +[](CallContract &v) { v.subscription->lease_ms = 0; },
    };
    for (const auto change : invalid_collections) {
        auto candidate = subscription;
        change(candidate);
        auto output = normalized;
        EXPECT_EQ(normalize_call_contract(candidate, output), AYNI_INVALID_ARGUMENT);
        EXPECT_TRUE(same_call_contract(output, normalized));
    }
}

// C-BIND-001 / C-SUB-001: a Provider replacement must preserve every
// collection source/lease and resource unit, even when all other fields agree.
TEST(CallContract, exact_compatibility_includes_collection_and_resource_semantics) {
    auto input = contract();
    input.form = CallForm::subscription;
    input.category = CallClass::tool;
    input.effects = 0;
    input.cancel = CancelMode::confirmed;
    input.subscription = SubscriptionContract{id("camera"), 1, 500};
    input.resource_count = 1;
    input.resources[0] = {id("camera.slot"), 1};
    CallContract original{};
    ASSERT_EQ(normalize_call_contract(input, original), AYNI_OK);
    const auto changes = {
        +[](CallContract &v) { v.subscription->source = id("different.camera"); },
        +[](CallContract &v) { ++v.subscription->source_version; },
        +[](CallContract &v) { ++v.subscription->lease_ms; },
        +[](CallContract &v) { v.resources[0].id = id("another.slot"); },
        +[](CallContract &v) { ++v.resources[0].units; },
    };
    for (const auto change : changes) {
        auto candidate = original;
        change(candidate);
        CallContract normalized{};
        ASSERT_EQ(normalize_call_contract(candidate, normalized), AYNI_OK);
        EXPECT_FALSE(same_call_contract(original, normalized));
        EXPECT_FALSE(same_call_contract(normalized, original));
    }
}

// ADR-0054: only a model proposes tool calls; a tool, an actuator or a
// subscription never declares the format.
TEST(CallContract, tool_call_results_belong_to_nonphysical_unary_or_action_models) {
    auto input = contract();
    input.result_format = call_tool_calls_result_format;
    CallContract output{};
    ASSERT_EQ(normalize_call_contract(input, output), AYNI_OK);
    EXPECT_EQ(output.result_format, call_tool_calls_result_format);
    output = {};
    input.category = CallClass::tool;
    EXPECT_EQ(normalize_call_contract(input, output), AYNI_INVALID_ARGUMENT);
    input = contract();
    input.result_format = call_tool_calls_result_format;
    input.effects = call_effect_physical;
    EXPECT_EQ(normalize_call_contract(input, output), AYNI_INVALID_ARGUMENT);
    input.effects = 0;
    input.form = CallForm::subscription;
    EXPECT_EQ(normalize_call_contract(input, output), AYNI_INVALID_ARGUMENT);
    input.form = CallForm::unary;
    input.result_format = call_value_result_format + 1U;
    EXPECT_EQ(normalize_call_contract(input, output), AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(output.timeout_ms, 0U);
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
    std::vector<uint64_t> storage(
        CallCapabilityRegistry::storage_size(slots, 4096) / sizeof(uint64_t) + 1U);
    CallCapabilityRegistry registry(storage.data(), slots, 4096);
    ayni::test::TextBusiness business(4096, 2048);
    CallCapability input{id("model.chat"), 1, contract()};
    ASSERT_EQ(registry.add_validated(input, &business.definition), AYNI_OK);
    EXPECT_EQ(registry.add_validated(input, &business.definition), AYNI_INVALID_ARGUMENT);
    input.version = 2;
    input.contract.result_text_limit = 1;
    ASSERT_EQ(registry.add_validated(input, &business.definition), AYNI_OK);
    input.version = 3;
    EXPECT_EQ(registry.add_validated(input, &business.definition), AYNI_CAPACITY_EXCEEDED);
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
