// Public registration/health/diagnostics contract, C-RES-013 and ADR-0042.
#include "kernel_node.h"

#include <ayni/resources.h>

#include <gtest/gtest.h>

namespace {
using ayni::test::Node;
const ayni_string_v1 model_id{"model", 5};

ayni_resource_desc_v1 model() {
    return {sizeof(ayni_resource_desc_v1), AYNI_RESOURCE_SHARED, model_id, 1, 0, 0, 0, nullptr, {}};
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
    node.config().device_capacity = AYNI_DEVICE_CAPACITY_MAX;
    node.config().component_capacity = AYNI_COMPONENT_CAPACITY_MAX;
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
    node.config().resource_request_capacity = 0;
    node.config().device_capacity += 1;
    EXPECT_EQ(ayni_kernel_memory_requirements(&node.config(), &full_size, &alignment),
              AYNI_INVALID_ARGUMENT);
    node.config().device_capacity = 0;
    node.config().component_capacity += 1;
    EXPECT_EQ(ayni_kernel_memory_requirements(&node.config(), &full_size, &alignment),
              AYNI_INVALID_ARGUMENT);
}
// C-RES-013/C-LIFE-003: trusted inventory is copied and resolves independent of registration order.
TEST(ResourceApi, device_component_resource_links_are_copied_and_resolved_at_start) {
    Node node;
    node.config().device_capacity = 1;
    node.config().component_capacity = 1;
    node.config().resource_capacity = 1;
    node.create();
    auto resource = model();
    const ayni_string_v1 components[] = {{"audio", 5}};
    resource.component_count = 1;
    resource.components = components;
    resource.conflict_domain_id = {"audio.bus", 9};
    ASSERT_EQ(ayni_register_resource(node.kernel(), &resource), AYNI_OK);
    EXPECT_EQ(ayni_start(node.kernel()), AYNI_NOT_FOUND);
    char parent[] = "native";
    const ayni_component_desc_v1 component{sizeof(component), 0, {"audio", 5}, {parent, 6}};
    ASSERT_EQ(ayni_register_component(node.kernel(), &component), AYNI_OK);
    parent[0] = 'x';
    EXPECT_EQ(ayni_start(node.kernel()), AYNI_NOT_FOUND);
    const ayni_device_desc_v1 device{sizeof(device), 0, {"native", 6}};
    ASSERT_EQ(ayni_register_device(node.kernel(), &device), AYNI_OK);
    ayni_device_info_v1 device_info{sizeof(device_info), 0, 0, 0};
    ASSERT_EQ(ayni_get_device(node.kernel(), device.device_id, &device_info), AYNI_OK);
    EXPECT_EQ(device_info.component_count, 1U);
    EXPECT_EQ(device_info.resource_count, 1U);
    ayni_component_info_v1 component_info{};
    component_info.struct_size = sizeof(component_info);
    ASSERT_EQ(ayni_get_component(node.kernel(), component.component_id, &component_info), AYNI_OK);
    EXPECT_EQ(std::string(component_info.device_id.data, component_info.device_id.size), "native");
    EXPECT_EQ(component_info.resource_count, 1U);
    const auto resource_info = info(node);
    EXPECT_EQ(std::string(resource_info.components[0].data, resource_info.components[0].size),
              "audio");
    EXPECT_EQ(
        std::string(resource_info.conflict_domain_id.data, resource_info.conflict_domain_id.size),
        "audio.bus");
    ASSERT_EQ(ayni_start(node.kernel()), AYNI_OK);
    node.recover();
    EXPECT_EQ(ayni_register_device(node.kernel(), &device), AYNI_INVALID_STATE);
    EXPECT_EQ(ayni_register_component(node.kernel(), &component), AYNI_INVALID_STATE);
    EXPECT_EQ(ayni_get_device(node.kernel(), device.device_id, &device_info), AYNI_OK);
}

// C-RES-013: disabled/full directories and malformed inputs never fabricate a partial inventory.
TEST(ResourceApi, inventory_capacity_and_shape_failures_preserve_existing_descriptions) {
    Node disabled;
    disabled.create();
    const ayni_device_desc_v1 device{sizeof(device), 0, {"native", 6}};
    EXPECT_EQ(ayni_register_device(disabled.kernel(), &device), AYNI_CAPACITY_EXCEEDED);
    const ayni_component_desc_v1 component{sizeof(component), 0, {"audio", 5}, {"native", 6}};
    EXPECT_EQ(ayni_register_component(disabled.kernel(), &component), AYNI_CAPACITY_EXCEEDED);
    Node node;
    node.config().device_capacity = 1;
    node.config().component_capacity = 1;
    node.create();
    auto bad = component;
    bad.device_id = {};
    EXPECT_EQ(ayni_register_component(node.kernel(), &bad), AYNI_INVALID_ARGUMENT);
    bad = component;
    bad.reserved = 1;
    EXPECT_EQ(ayni_register_component(node.kernel(), &bad), AYNI_INVALID_ARGUMENT);
    ASSERT_EQ(ayni_register_component(node.kernel(), &component), AYNI_OK);
    EXPECT_EQ(ayni_register_component(node.kernel(), &component), AYNI_INVALID_ARGUMENT);
    bad = component;
    bad.component_id = {"other", 5};
    EXPECT_EQ(ayni_register_component(node.kernel(), &bad), AYNI_CAPACITY_EXCEEDED);
    ASSERT_EQ(ayni_register_device(node.kernel(), &device), AYNI_OK);
    auto next = device;
    next.device_id = {"other", 5};
    EXPECT_EQ(ayni_register_device(node.kernel(), &next), AYNI_CAPACITY_EXCEEDED);
    ayni_device_info_v1 copied{sizeof(copied), 7, 8, 9};
    EXPECT_EQ(ayni_get_device(node.kernel(), next.device_id, &copied), AYNI_NOT_FOUND);
    EXPECT_EQ(copied.component_count, 7U);
    EXPECT_EQ(ayni_get_device(node.kernel(), device.device_id, &copied), AYNI_OK);
    EXPECT_EQ(copied.component_count, 1U);
    EXPECT_EQ(copied.resource_count, 0U);
}
// ADR-0046: one chassis spans two components; multiple resources may name either one.
TEST(ResourceApi, shared_component_associations_are_copied_and_device_counts_are_unique) {
    Node node;
    node.config().device_capacity = 1;
    node.config().component_capacity = 2;
    node.config().resource_capacity = 2;
    node.create();
    const ayni_device_desc_v1 device{sizeof(device), 0, {"robot", 5}};
    ASSERT_EQ(ayni_register_device(node.kernel(), &device), AYNI_OK);
    char left[] = "left";
    const ayni_string_v1 components[] = {{left, 4}, {"right", 5}};
    for (const auto &id : components) {
        const ayni_component_desc_v1 component{sizeof(component), 0, id, device.device_id};
        ASSERT_EQ(ayni_register_component(node.kernel(), &component), AYNI_OK);
    }
    auto resource = model();
    resource.component_count = 2;
    resource.components = components;
    ASSERT_EQ(ayni_register_resource(node.kernel(), &resource), AYNI_OK);
    resource.resource_id = {"diagnostic", 10};
    resource.component_count = 1;
    ASSERT_EQ(ayni_register_resource(node.kernel(), &resource), AYNI_OK);
    left[0] = 'x';
    ayni_device_info_v1 device_info{sizeof(device_info), 0, 0, 0};
    ASSERT_EQ(ayni_get_device(node.kernel(), device.device_id, &device_info), AYNI_OK);
    EXPECT_EQ(device_info.component_count, 2U);
    EXPECT_EQ(device_info.resource_count, 2U);
    ayni_component_info_v1 component_info{};
    component_info.struct_size = sizeof(component_info);
    ASSERT_EQ(ayni_get_component(node.kernel(), {"left", 4}, &component_info), AYNI_OK);
    EXPECT_EQ(component_info.resource_count, 2U);
    ASSERT_EQ(ayni_get_component(node.kernel(), {"right", 5}, &component_info), AYNI_OK);
    EXPECT_EQ(component_info.resource_count, 1U);
    const auto snapshot = info(node);
    ASSERT_EQ(snapshot.component_count, 2U);
    EXPECT_EQ(std::string(snapshot.components[0].data, snapshot.components[0].size), "left");
    ASSERT_EQ(ayni_start(node.kernel()), AYNI_OK);
}

// ADR-0046: invalid association sets do not reserve a catalog slot or retain caller pointers.
TEST(ResourceApi, component_association_shape_and_capacity_are_checked_before_registration) {
    Node node;
    node.config().resource_capacity = 1;
    node.create();
    auto resource = model();
    resource.component_count = 1;
    EXPECT_EQ(ayni_register_resource(node.kernel(), &resource), AYNI_INVALID_ARGUMENT);
    resource.component_count = AYNI_RESOURCE_COMPONENT_COUNT_MAX + 1U;
    EXPECT_EQ(ayni_register_resource(node.kernel(), &resource), AYNI_INVALID_ARGUMENT);
    const ayni_string_v1 duplicates[] = {{"left", 4}, {"left", 4}};
    resource.components = duplicates;
    resource.component_count = 2;
    EXPECT_EQ(ayni_register_resource(node.kernel(), &resource), AYNI_INVALID_ARGUMENT);
    resource = model();
    resource.reserved = 1;
    EXPECT_EQ(ayni_register_resource(node.kernel(), &resource), AYNI_INVALID_ARGUMENT);
    resource = model();
    ASSERT_EQ(ayni_register_resource(node.kernel(), &resource), AYNI_OK);
    EXPECT_EQ(info(node).component_count, 0U);
}
} // namespace
