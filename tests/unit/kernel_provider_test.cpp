// Public C ABI catalog/startup scenarios, C-BIND-001/C-PROV-001, ADR-0045.
// No registration or compatibility check may perform a Provider request.
#include "kernel_node.h"

#include <ayni/calls.h>

#include <gtest/gtest.h>

namespace {
using ayni::test::Node;

ayni_submit_result_t reject(void *ctx, const ayni_provider_request_v1 *,
                            ayni_provider_completion_t *) {
    ++*static_cast<unsigned *>(ctx);
    return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}
ayni_submit_result_t cancel(void *, ayni_attempt_ref_v1) {
    return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}

class ProviderApi : public ::testing::Test {
  protected:
    void SetUp() override {
        node.config().call_capability_capacity = 2;
        node.config().provider_capacity = 2;
        node.config().binding_capacity = 2;
        node.config().resource_capacity = 2;
        node.create();
        capability = {};
        capability.struct_size = sizeof(capability);
        capability.version = 1;
        capability.capability_id = {"echo", 4};
        auto &contract = capability.contract;
        contract.struct_size = sizeof(contract);
        contract.form = AYNI_CALL_UNARY;
        contract.category = AYNI_CALL_TOOL;
        contract.idempotency = AYNI_CALL_IDEMPOTENT;
        contract.query = AYNI_CALL_QUERY_NONE;
        contract.cancel = AYNI_CALL_CANCEL_NONE;
        contract.timeout_ms = 30000;
        contract.input_format = AYNI_CALL_INPUT_FIELDS_TEXT;
        contract.result_format = AYNI_CALL_RESULT_TEXT;
        contract.input_text_limit = 1024;
        contract.result_text_limit = 2048;
        provider = {};
        provider.struct_size = sizeof(provider);
        provider.version = 1;
        provider.provider_id = {"mock.a", 6};
        provider.endpoint_id = {"mock.echo", 9};
        provider.ctx = &submissions;
        provider.capability_count = 1;
        provider.capabilities = &capability;
        provider.submit = &reject;
        binding = {sizeof(binding),
                   1,
                   {"echo.a", 6},
                   capability.capability_id,
                   1,
                   1,
                   provider.provider_id,
                   provider.endpoint_id,
                   {nullptr, 0}};
    }

    void authority(const ayni_authority_field_desc_v1 *fields = nullptr, uint32_t count = 0) {
        const ayni_capability_desc_v1 desc{
            sizeof(desc), capability.version, capability.capability_id, count, 0, fields};
        ASSERT_EQ(ayni_register_capability(node.kernel(), &desc), AYNI_OK);
    }
    void register_all() {
        ASSERT_EQ(ayni_register_binding(node.kernel(), &binding), AYNI_OK);
        ASSERT_EQ(ayni_register_provider(node.kernel(), &provider), AYNI_OK);
        ASSERT_EQ(ayni_register_call_capability(node.kernel(), &capability), AYNI_OK);
        authority();
    }

    unsigned submissions = 0;
    Node node;
    ayni_call_capability_v1 capability{};
    ayni_provider_v1 provider{};
    ayni_binding_v1 binding{};
};

TEST_F(ProviderApi, copied_registration_resolves_in_any_order_without_invoking_callbacks) {
    char name[] = "echo";
    capability.capability_id = {name, 4};
    register_all();
    name[0] = 'x';
    provider.endpoint_id = {"changed", 7};
    capability.contract.timeout_ms = 1;
    ASSERT_EQ(ayni_start(node.kernel()), AYNI_OK);
    node.recover();
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(submissions, 0U);
    EXPECT_EQ(ayni_register_binding(node.kernel(), &binding), AYNI_INVALID_STATE);
    EXPECT_EQ(ayni_register_provider(node.kernel(), &provider), AYNI_INVALID_STATE);
    EXPECT_EQ(ayni_register_call_capability(node.kernel(), &capability), AYNI_INVALID_STATE);
}

TEST_F(ProviderApi, missing_reference_keeps_configuring_and_can_be_supplied_before_retry) {
    ASSERT_EQ(ayni_register_binding(node.kernel(), &binding), AYNI_OK);
    ASSERT_EQ(ayni_start(node.kernel()), AYNI_NOT_FOUND);
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_CONFIGURING);
    EXPECT_EQ(node.sim().pending_storage(), 0U);
    authority();
    ASSERT_EQ(ayni_register_call_capability(node.kernel(), &capability), AYNI_OK);
    ASSERT_EQ(ayni_register_provider(node.kernel(), &provider), AYNI_OK);
    ASSERT_EQ(ayni_start(node.kernel()), AYNI_OK);
    EXPECT_EQ(submissions, 0U);
}

TEST_F(ProviderApi, provider_cannot_weaken_the_capability_contract) {
    authority();
    ASSERT_EQ(ayni_register_call_capability(node.kernel(), &capability), AYNI_OK);
    --capability.contract.result_text_limit;
    ASSERT_EQ(ayni_register_provider(node.kernel(), &provider), AYNI_OK);
    ASSERT_EQ(ayni_register_binding(node.kernel(), &binding), AYNI_OK);
    EXPECT_EQ(ayni_start(node.kernel()), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_CONFIGURING);
    EXPECT_EQ(submissions, 0U);
}

TEST_F(ProviderApi, binding_does_not_silently_select_another_provider_version_or_endpoint) {
    binding.provider_version = 2;
    register_all();
    EXPECT_EQ(ayni_start(node.kernel()), AYNI_NOT_FOUND);
    provider.version = 2;
    provider.endpoint_id = {"different", 9};
    ASSERT_EQ(ayni_register_provider(node.kernel(), &provider), AYNI_OK);
    EXPECT_EQ(ayni_start(node.kernel()), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(submissions, 0U);
}

TEST_F(ProviderApi, offline_resources_link_but_an_impossible_group_is_rejected) {
    const ayni_call_resource_v1 demand{{"slot", 4}, 2, 0};
    capability.contract.resources = &demand;
    capability.contract.resource_count = 1;
    register_all();
    EXPECT_EQ(ayni_start(node.kernel()), AYNI_NOT_FOUND);
    const ayni_resource_desc_v1 resource{sizeof(resource), AYNI_RESOURCE_SHARED, demand.resource_id,
                                         1, 0};
    ASSERT_EQ(ayni_register_resource(node.kernel(), &resource), AYNI_OK);
    EXPECT_EQ(ayni_start(node.kernel()), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(submissions, 0U);
}

TEST_F(ProviderApi, canonical_resource_order_does_not_change_offer_compatibility) {
    ayni_call_resource_v1 demands[] = {{{"a", 1}, 1, 0}, {{"b", 1}, 1, 0}};
    capability.contract.resources = demands;
    capability.contract.resource_count = 2;
    authority();
    ASSERT_EQ(ayni_register_call_capability(node.kernel(), &capability), AYNI_OK);
    std::swap(demands[0], demands[1]);
    ASSERT_EQ(ayni_register_provider(node.kernel(), &provider), AYNI_OK);
    ASSERT_EQ(ayni_register_binding(node.kernel(), &binding), AYNI_OK);
    for (const auto &demand : demands) {
        const ayni_resource_desc_v1 resource{sizeof(resource), AYNI_RESOURCE_SHARED,
                                             demand.resource_id, 1, 0};
        ASSERT_EQ(ayni_register_resource(node.kernel(), &resource), AYNI_OK);
    }
    ASSERT_EQ(ayni_start(node.kernel()), AYNI_OK);
    node.recover();
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(submissions, 0U);
}

TEST_F(ProviderApi, egress_destination_must_belong_to_the_authority_domain) {
    capability.contract.effects = AYNI_CALL_EFFECT_EGRESS;
    const ayni_string_v1 destination{"service.a", 9};
    provider.destination_id = destination;
    binding.destination_id = destination;
    const ayni_string_v1 allowed{"service.b", 9};
    ayni_authority_field_desc_v1 field{};
    field.struct_size = sizeof(field);
    field.field_id = 1;
    field.usage = AYNI_AUTHORITY_EGRESS_DESTINATION;
    field.domain.struct_size = sizeof(field.domain);
    field.domain.kind = AYNI_AUTHORITY_IDENTIFIER;
    field.domain.values.tokens = {&allowed, 1, 0};
    authority(&field, 1);
    ASSERT_EQ(ayni_register_call_capability(node.kernel(), &capability), AYNI_OK);
    ASSERT_EQ(ayni_register_provider(node.kernel(), &provider), AYNI_OK);
    ASSERT_EQ(ayni_register_binding(node.kernel(), &binding), AYNI_OK);
    EXPECT_EQ(ayni_start(node.kernel()), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(submissions, 0U);
}

TEST_F(ProviderApi, externally_effectful_offer_needs_destination_and_matching_cancel_port) {
    capability.contract.effects = AYNI_CALL_EFFECT_EGRESS;
    EXPECT_EQ(ayni_register_provider(node.kernel(), &provider), AYNI_INVALID_ARGUMENT);
    provider.destination_id = {"service.a", 9};
    capability.contract.cancel = AYNI_CALL_CANCEL_REQUEST;
    EXPECT_EQ(ayni_register_provider(node.kernel(), &provider), AYNI_INVALID_ARGUMENT);
    provider.cancel = &cancel;
    EXPECT_EQ(ayni_register_provider(node.kernel(), &provider), AYNI_OK);
    EXPECT_EQ(ayni_register_provider(node.kernel(), &provider), AYNI_INVALID_ARGUMENT);
}

TEST_F(ProviderApi, duplicate_offers_are_atomic_and_unknown_formats_are_not_defaulted) {
    ayni_call_capability_v1 offers[]{capability, capability};
    provider.capabilities = offers;
    provider.capability_count = 2;
    EXPECT_EQ(ayni_register_provider(node.kernel(), &provider), AYNI_INVALID_ARGUMENT);
    offers[1].version = 2;
    offers[1].contract.input_format = 0;
    EXPECT_EQ(ayni_register_provider(node.kernel(), &provider), AYNI_UNSUPPORTED_VERSION);
    offers[1].contract.input_format = AYNI_CALL_INPUT_FIELDS_TEXT;
    EXPECT_EQ(ayni_register_provider(node.kernel(), &provider), AYNI_OK);
    ++provider.version;
    EXPECT_EQ(ayni_register_provider(node.kernel(), &provider), AYNI_OK);
    ++provider.version;
    EXPECT_EQ(ayni_register_provider(node.kernel(), &provider), AYNI_CAPACITY_EXCEEDED);
}

TEST_F(ProviderApi, disabled_directories_fail_explicitly_and_configuration_bounds_are_checked) {
    Node disabled;
    disabled.create();
    EXPECT_EQ(ayni_register_call_capability(disabled.kernel(), &capability),
              AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(ayni_register_provider(disabled.kernel(), &provider), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(ayni_register_binding(disabled.kernel(), &binding), AYNI_CAPACITY_EXCEEDED);
    size_t size = 0;
    size_t alignment = 0;
    disabled.config().provider_capacity = AYNI_PROVIDER_CAPACITY_MAX + 1;
    EXPECT_EQ(ayni_kernel_memory_requirements(&disabled.config(), &size, &alignment),
              AYNI_INVALID_ARGUMENT);
}
} // namespace
