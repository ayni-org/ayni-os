// Capability registration is configuration metadata, never authorization or I/O.
#include "kernel_node.h"

#include <ayni/authority.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace {
using namespace ayni::test;

ayni_capability_desc_v1 capability(uint32_t version = 1) {
    ayni_capability_desc_v1 result{};
    result.struct_size = sizeof(result);
    result.version = version;
    result.capability_id = {"notify.present", 14};
    return result;
}

TEST(CapabilityApi, registration_is_bounded_copies_identity_and_never_commits_or_wakes) {
    Node node;
    node.config().capability_capacity = 1;
    node.create();
    const auto before = node.info();
    const auto wakes = node.sim().wake_count();
    auto first = capability();
    char identity[] = "notify.present";
    first.capability_id = {identity, 14};
    ASSERT_EQ(ayni_register_capability(node.kernel(), &first), AYNI_OK);
    identity[0] = 'x';
    auto duplicate = capability();
    EXPECT_EQ(ayni_register_capability(node.kernel(), &duplicate), AYNI_INVALID_ARGUMENT);
    auto second_version = capability(2);
    EXPECT_EQ(ayni_register_capability(node.kernel(), &second_version), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(node.info().boot_epoch, before.boot_epoch);
    EXPECT_EQ(node.info().durable_seq, before.durable_seq);
    EXPECT_EQ(node.sim().wake_count(), wakes);
    EXPECT_EQ(node.sim().pending_storage(), 0U);
    EXPECT_TRUE(node.sim().log().empty());
}

TEST(CapabilityApi, malformed_descriptors_do_not_consume_the_only_registration_slot) {
    Node node;
    node.config().capability_capacity = 1;
    node.create();
    EXPECT_EQ(ayni_register_capability(node.kernel(), nullptr), AYNI_INVALID_ARGUMENT);
    auto value = capability();
    EXPECT_EQ(ayni_register_capability(nullptr, &value), AYNI_INVALID_ARGUMENT);
    value.struct_size -= 1;
    EXPECT_EQ(ayni_register_capability(node.kernel(), &value), AYNI_INVALID_ARGUMENT);
    value = capability();
    value.struct_size += 1;
    EXPECT_EQ(ayni_register_capability(node.kernel(), &value), AYNI_UNSUPPORTED_VERSION);
    value = capability();
    value.reserved = 1;
    EXPECT_EQ(ayni_register_capability(node.kernel(), &value), AYNI_INVALID_ARGUMENT);
    value = capability();
    value.field_count = AYNI_AUTHORITY_FIELD_COUNT_MAX + 1;
    EXPECT_EQ(ayni_register_capability(node.kernel(), &value), AYNI_INVALID_ARGUMENT);
    value = capability();
    ASSERT_EQ(ayni_register_capability(node.kernel(), &value), AYNI_OK);
}

TEST(CapabilityApi, an_exact_capability_version_may_coexist_but_registration_freezes_at_start) {
    Node node;
    node.create();
    auto first = capability();
    auto second = capability(2);
    ASSERT_EQ(ayni_register_capability(node.kernel(), &first), AYNI_OK);
    ASSERT_EQ(ayni_register_capability(node.kernel(), &second), AYNI_OK);
    ASSERT_EQ(ayni_start(node.kernel()), AYNI_OK);
    EXPECT_EQ(ayni_register_capability(node.kernel(), &first), AYNI_INVALID_STATE);
    node.recover();
    EXPECT_EQ(ayni_register_capability(node.kernel(), &first), AYNI_INVALID_STATE);
    auto malformed = first;
    malformed.field_count = 1;
    malformed.fields = nullptr;
    EXPECT_EQ(ayni_register_capability(node.kernel(), &malformed), AYNI_INVALID_ARGUMENT);
    ASSERT_EQ(ayni_begin_shutdown(node.kernel()), AYNI_OK);
    EXPECT_EQ(ayni_register_capability(node.kernel(), &first), AYNI_INVALID_STATE);
}

} // namespace
