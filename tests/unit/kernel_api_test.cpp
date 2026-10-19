// Contract of the M0 C ABI in include/ayni/ayni.h: version query, memory
// requirements, create, lifecycle query and destroy.

#include <ayni/ayni.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

uint64_t fixed_clock(void * /*ctx*/) {
    return 42;
}

ayni_config_v1 valid_config() {
    ayni_config_v1 config{};
    config.struct_size = sizeof(config);
    config.abi_major = AYNI_ABI_MAJOR;
    return config;
}

ayni_host_v1 valid_host() {
    ayni_host_v1 host{};
    host.struct_size = sizeof(host);
    host.monotonic_now_us = &fixed_clock;
    return host;
}

// Caller-provided kernel memory, over-sized and over-aligned so tests can
// also hand out misaligned or short views of it.
struct KernelMemory {
    alignas(64) std::array<unsigned char, 1024> bytes{};
    std::size_t size = 0;
    std::size_t align = 0;
};

class KernelApi : public ::testing::Test {
  protected:
    void SetUp() override {
        const ayni_config_v1 config = valid_config();
        ASSERT_EQ(ayni_kernel_memory_requirements(&config, &memory_.size, &memory_.align), AYNI_OK);
        ASSERT_LE(memory_.size, memory_.bytes.size());
        ASSERT_LE(memory_.align, std::size_t{64});
    }

    ayni_status_t create(const ayni_config_v1 *config, const ayni_host_v1 *host,
                         ayni_kernel_t **out_kernel) {
        return ayni_create(config, host, memory_.bytes.data(), memory_.size, out_kernel);
    }

    KernelMemory memory_;
};

TEST(AbiVersion, reports_header_version) {
    uint32_t major = 99;
    uint32_t minor = 99;
    ASSERT_EQ(ayni_get_abi_version(&major, &minor), AYNI_OK);
    EXPECT_EQ(major, AYNI_ABI_MAJOR);
    EXPECT_EQ(minor, AYNI_ABI_MINOR);
}

TEST(AbiVersion, rejects_null_outputs) {
    uint32_t value = 0;
    EXPECT_EQ(ayni_get_abi_version(nullptr, &value), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_get_abi_version(&value, nullptr), AYNI_INVALID_ARGUMENT);
}

TEST_F(KernelApi, memory_requirements_report_power_of_two_alignment) {
    EXPECT_GT(memory_.size, 0U);
    ASSERT_GT(memory_.align, 0U);
    EXPECT_EQ(memory_.align & (memory_.align - 1), 0U);
}

TEST_F(KernelApi, memory_requirements_reject_invalid_config) {
    std::size_t size = 0;
    std::size_t align = 0;
    EXPECT_EQ(ayni_kernel_memory_requirements(nullptr, &size, &align), AYNI_INVALID_ARGUMENT);

    ayni_config_v1 config = valid_config();
    EXPECT_EQ(ayni_kernel_memory_requirements(&config, nullptr, &align), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_kernel_memory_requirements(&config, &size, nullptr), AYNI_INVALID_ARGUMENT);

    config.abi_major = AYNI_ABI_MAJOR + 1;
    EXPECT_EQ(ayni_kernel_memory_requirements(&config, &size, &align), AYNI_UNSUPPORTED_VERSION);
}

TEST_F(KernelApi, create_in_caller_memory_starts_configuring) {
    const ayni_config_v1 config = valid_config();
    const ayni_host_v1 host = valid_host();
    ayni_kernel_t *kernel = nullptr;
    ASSERT_EQ(create(&config, &host, &kernel), AYNI_OK);
    ASSERT_NE(kernel, nullptr);
    EXPECT_EQ(static_cast<void *>(kernel), static_cast<void *>(memory_.bytes.data()));

    ayni_lifecycle_t state = 0;
    ASSERT_EQ(ayni_get_lifecycle(kernel, &state), AYNI_OK);
    EXPECT_EQ(state, AYNI_LIFECYCLE_CONFIGURING);
    EXPECT_EQ(ayni_destroy(kernel), AYNI_OK);
}

TEST_F(KernelApi, create_copies_host_table_before_return) {
    const ayni_config_v1 config = valid_config();
    ayni_kernel_t *kernel = nullptr;
    {
        ayni_host_v1 host = valid_host();
        ASSERT_EQ(create(&config, &host, &kernel), AYNI_OK);
        std::memset(&host, 0xA5, sizeof(host));
    }
    ayni_lifecycle_t state = 0;
    EXPECT_EQ(ayni_get_lifecycle(kernel, &state), AYNI_OK);
    EXPECT_EQ(ayni_destroy(kernel), AYNI_OK);
}

TEST_F(KernelApi, create_rejects_null_arguments) {
    const ayni_config_v1 config = valid_config();
    const ayni_host_v1 host = valid_host();
    ayni_kernel_t *kernel = nullptr;
    EXPECT_EQ(create(&config, &host, nullptr), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(create(nullptr, &host, &kernel), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(create(&config, nullptr, &kernel), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_create(&config, &host, nullptr, memory_.size, &kernel), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(kernel, nullptr);
}

TEST_F(KernelApi, create_rejects_missing_clock) {
    const ayni_config_v1 config = valid_config();
    ayni_host_v1 host = valid_host();
    host.monotonic_now_us = nullptr;
    ayni_kernel_t *kernel = nullptr;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT);
}

TEST_F(KernelApi, struct_size_below_first_layout_is_invalid_argument) {
    ayni_config_v1 config = valid_config();
    ayni_host_v1 host = valid_host();
    ayni_kernel_t *kernel = nullptr;

    config.struct_size = sizeof(config) - 1;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT);
    config = valid_config();
    host.struct_size = sizeof(host) - 1;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT);
}

TEST_F(KernelApi, struct_size_from_newer_minor_is_unsupported_version) {
    ayni_config_v1 config = valid_config();
    ayni_host_v1 host = valid_host();
    ayni_kernel_t *kernel = nullptr;

    config.struct_size = sizeof(config) + 8;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_UNSUPPORTED_VERSION);
    config = valid_config();
    host.struct_size = sizeof(host) + 8;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_UNSUPPORTED_VERSION);
}

TEST_F(KernelApi, create_rejects_other_abi_major) {
    ayni_config_v1 config = valid_config();
    config.abi_major = AYNI_ABI_MAJOR + 1;
    const ayni_host_v1 host = valid_host();
    ayni_kernel_t *kernel = nullptr;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_UNSUPPORTED_VERSION);
}

TEST_F(KernelApi, create_rejects_misaligned_memory) {
    if (memory_.align == 1) {
        GTEST_SKIP() << "kernel memory needs no alignment on this target";
    }
    const ayni_config_v1 config = valid_config();
    const ayni_host_v1 host = valid_host();
    ayni_kernel_t *kernel = nullptr;
    EXPECT_EQ(ayni_create(&config, &host, memory_.bytes.data() + 1, memory_.size, &kernel),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(kernel, nullptr);
}

TEST_F(KernelApi, create_rejects_memory_below_requirement) {
    const ayni_config_v1 config = valid_config();
    const ayni_host_v1 host = valid_host();
    ayni_kernel_t *kernel = nullptr;
    EXPECT_EQ(ayni_create(&config, &host, memory_.bytes.data(), memory_.size - 1, &kernel),
              AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(kernel, nullptr);
}

TEST_F(KernelApi, failed_create_leaves_memory_untouched) {
    std::fill(memory_.bytes.begin(), memory_.bytes.end(), static_cast<unsigned char>(0x5A));
    ayni_config_v1 config = valid_config();
    config.abi_major = AYNI_ABI_MAJOR + 1;
    const ayni_host_v1 host = valid_host();
    ayni_kernel_t *kernel = reinterpret_cast<ayni_kernel_t *>(&config);

    EXPECT_EQ(create(&config, &host, &kernel), AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(kernel, nullptr);
    EXPECT_TRUE(std::all_of(memory_.bytes.begin(), memory_.bytes.end(),
                            [](unsigned char byte) { return byte == 0x5A; }));
}

TEST_F(KernelApi, kernel_memory_is_reusable_after_destroy) {
    const ayni_config_v1 config = valid_config();
    const ayni_host_v1 host = valid_host();
    for (int round = 0; round < 3; ++round) {
        ayni_kernel_t *kernel = nullptr;
        ASSERT_EQ(create(&config, &host, &kernel), AYNI_OK);
        ASSERT_EQ(ayni_destroy(kernel), AYNI_OK);
    }
}

TEST(KernelHandle, null_kernel_is_invalid_argument) {
    ayni_lifecycle_t state = 0;
    EXPECT_EQ(ayni_get_lifecycle(nullptr, &state), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_destroy(nullptr), AYNI_INVALID_ARGUMENT);
}

TEST_F(KernelApi, get_lifecycle_rejects_null_output) {
    const ayni_config_v1 config = valid_config();
    const ayni_host_v1 host = valid_host();
    ayni_kernel_t *kernel = nullptr;
    ASSERT_EQ(create(&config, &host, &kernel), AYNI_OK);
    EXPECT_EQ(ayni_get_lifecycle(kernel, nullptr), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_destroy(kernel), AYNI_OK);
}

} // namespace
