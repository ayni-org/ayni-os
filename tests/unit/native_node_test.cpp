#include <ayni/runtime/native_node.h>

#include <gtest/gtest.h>

namespace {

TEST(NativeNode, assemble_creates_configuring_kernel) {
    ayni::runtime::NativeNode node;
    ASSERT_EQ(node.assemble(), AYNI_OK);
    ASSERT_NE(node.kernel(), nullptr);

    ayni_lifecycle_t state = 0;
    ASSERT_EQ(ayni_get_lifecycle(node.kernel(), &state), AYNI_OK);
    EXPECT_EQ(state, AYNI_LIFECYCLE_CONFIGURING);
    EXPECT_EQ(node.release(), AYNI_OK);
    EXPECT_EQ(node.kernel(), nullptr);
}

TEST(NativeNode, second_assemble_is_invalid_state) {
    ayni::runtime::NativeNode node;
    ASSERT_EQ(node.assemble(), AYNI_OK);
    ayni_kernel_t *const first = node.kernel();
    EXPECT_EQ(node.assemble(), AYNI_INVALID_STATE);
    EXPECT_EQ(node.kernel(), first);
}

TEST(NativeNode, release_without_kernel_is_invalid_state) {
    ayni::runtime::NativeNode node;
    EXPECT_EQ(node.release(), AYNI_INVALID_STATE);
}

TEST(NativeNode, assemble_after_release_creates_a_new_kernel) {
    ayni::runtime::NativeNode node;
    for (int round = 0; round < 3; ++round) {
        ASSERT_EQ(node.assemble(), AYNI_OK);
        ASSERT_EQ(node.release(), AYNI_OK);
    }
}

TEST(NativeNode, destructor_releases_assembled_kernel) {
    // Leak detection (ASan in CI) proves the memory is returned.
    ayni::runtime::NativeNode node;
    ASSERT_EQ(node.assemble(), AYNI_OK);
}

} // namespace
