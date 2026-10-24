// Public registration/health/diagnostics contract, C-RES-013 and ADR-0042.
#include "kernel_node.h"

#include <ayni/resources.h>

#include <gtest/gtest.h>

namespace {
using ayni::test::Node;
const ayni_string_v1 model_id{"model", 5};

ayni_resource_desc_v1 model() {
    return {sizeof(ayni_resource_desc_v1), AYNI_RESOURCE_SHARED, model_id, 1, 0};
}
ayni_resource_info_v1 info(Node &node) {
    ayni_resource_info_v1 result{};
    result.struct_size = sizeof(result);
    EXPECT_EQ(ayni_get_resource(node.kernel(), model_id, &result), AYNI_OK);
    return result;
}

TEST(ResourceApi, registration_copies_trusted_metadata_and_health_never_commits_storage) {
    Node node;
    node.config().resource_capacity = 1;
    node.create();
    auto descriptor = model();
    char name[] = "model";
    descriptor.resource_id = {name, 5};
    ASSERT_EQ(ayni_register_resource(node.kernel(), &descriptor), AYNI_OK);
    name[0] = 'x';
    EXPECT_EQ(info(node).state, AYNI_RESOURCE_UNAVAILABLE);
    EXPECT_EQ(info(node).health, AYNI_RESOURCE_OFFLINE);
    const auto wakes = node.sim().wake_count();
    ASSERT_EQ(ayni_set_resource_health(node.kernel(), model_id, AYNI_RESOURCE_ONLINE), AYNI_OK);
    EXPECT_EQ(info(node).state, AYNI_RESOURCE_FREE);
    EXPECT_EQ(node.sim().wake_count(), wakes + 1U);
    ASSERT_EQ(ayni_set_resource_health(node.kernel(), model_id, AYNI_RESOURCE_ONLINE), AYNI_OK);
    EXPECT_EQ(node.sim().wake_count(), wakes + 1U);
    EXPECT_EQ(node.sim().pending_storage(), 0U);
    EXPECT_TRUE(node.sim().log().empty());
    descriptor = model();
    EXPECT_EQ(ayni_register_resource(node.kernel(), &descriptor), AYNI_INVALID_ARGUMENT);
    descriptor.resource_id = {"audio", 5};
    EXPECT_EQ(ayni_register_resource(node.kernel(), &descriptor), AYNI_CAPACITY_EXCEEDED);
    ASSERT_EQ(ayni_start(node.kernel()), AYNI_OK);
    EXPECT_EQ(ayni_register_resource(node.kernel(), &descriptor), AYNI_INVALID_STATE);
    EXPECT_EQ(ayni_set_resource_health(node.kernel(), model_id, AYNI_RESOURCE_OFFLINE),
              AYNI_INVALID_STATE);
    node.recover();
    EXPECT_EQ(ayni_set_resource_health(node.kernel(), model_id, AYNI_RESOURCE_FAULTED), AYNI_OK);
    EXPECT_EQ(info(node).health, AYNI_RESOURCE_FAULTED);
}

TEST(ResourceApi, disabled_tables_and_invalid_descriptors_fail_explicitly) {
    Node disabled;
    disabled.create();
    auto descriptor = model();
    EXPECT_EQ(ayni_register_resource(disabled.kernel(), &descriptor), AYNI_CAPACITY_EXCEEDED);
    Node node;
    node.config().resource_capacity = 1;
    node.create();
    descriptor.flags = AYNI_RESOURCE_ACTUATOR;
    EXPECT_EQ(ayni_register_resource(node.kernel(), &descriptor), AYNI_INVALID_ARGUMENT);
    descriptor = model();
    descriptor.kind = AYNI_RESOURCE_EXCLUSIVE;
    descriptor.capacity = 2;
    EXPECT_EQ(ayni_register_resource(node.kernel(), &descriptor), AYNI_INVALID_ARGUMENT);
    descriptor = model();
    descriptor.struct_size += 1;
    EXPECT_EQ(ayni_register_resource(node.kernel(), &descriptor), AYNI_UNSUPPORTED_VERSION);
    descriptor = model();
    descriptor.struct_size -= 1;
    EXPECT_EQ(ayni_register_resource(node.kernel(), &descriptor), AYNI_INVALID_ARGUMENT);
    descriptor = model();
    ASSERT_EQ(ayni_register_resource(node.kernel(), &descriptor), AYNI_OK);
    const auto before = info(node);
    EXPECT_EQ(ayni_set_resource_health(node.kernel(), model_id, 0), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(info(node).health, before.health);
    auto preserved = before;
    EXPECT_EQ(ayni_get_resource(node.kernel(), {"missing", 7}, &preserved), AYNI_NOT_FOUND);
    EXPECT_EQ(preserved.health, before.health);
    EXPECT_EQ(ayni_get_resource(node.kernel(), model_id, nullptr), AYNI_INVALID_ARGUMENT);
}

TEST(ResourceApi, configured_capacities_are_bounded_and_arena_size_accounts_for_both_tables) {
    Node node;
    size_t base_size = 0;
    size_t alignment = 0;
    ASSERT_EQ(ayni_kernel_memory_requirements(&node.config(), &base_size, &alignment), AYNI_OK);
    node.config().resource_capacity = AYNI_RESOURCE_CAPACITY_MAX;
    node.config().resource_request_capacity = AYNI_RESOURCE_REQUEST_CAPACITY_MAX;
    size_t full_size = 0;
    ASSERT_EQ(ayni_kernel_memory_requirements(&node.config(), &full_size, &alignment), AYNI_OK);
    EXPECT_GT(full_size, base_size);
    node.create();
    node.config().resource_capacity += 1;
    EXPECT_EQ(ayni_kernel_memory_requirements(&node.config(), &full_size, &alignment),
              AYNI_INVALID_ARGUMENT);
    node.config().resource_capacity = 0;
    node.config().resource_request_capacity += 1;
    EXPECT_EQ(ayni_kernel_memory_requirements(&node.config(), &full_size, &alignment),
              AYNI_INVALID_ARGUMENT);
}
} // namespace
