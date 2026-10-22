// Contract of the C ABI in include/ayni/ayni.h: version query, memory
// requirements, create, lifecycle query and destroy. Completion posting is
// covered by completion_pool_test.cpp; start, step, shutdown and kernel info
// by kernel_lifecycle_test.cpp.

#include "kernel/kernel_state.h"
#include "kernel_node.h"

#include <ayni/ayni.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

uint64_t fixed_clock(void * /*ctx*/) {
    return 42;
}

void no_wake(void * /*ctx*/) {}
void no_lock(void * /*ctx*/) {}
void no_unlock(void * /*ctx*/) {}

ayni_submit_result_t reject_append(void * /*ctx*/, const ayni_storage_batch_v1 * /*batch*/,
                                   ayni_attempt_ref_v1 /*attempt*/,
                                   ayni_completion_t * /*completion*/) {
    return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}

ayni_submit_result_t reject_read(void * /*ctx*/, uint64_t /*from_commit_seq*/, uint8_t * /*buffer*/,
                                 uint32_t /*buffer_size*/, ayni_attempt_ref_v1 /*attempt*/,
                                 ayni_completion_t * /*completion*/) {
    return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}

// Small capacities keep the kernel inside KernelMemory.
ayni_config_v1 valid_config() {
    ayni_config_v1 config{};
    config.struct_size = sizeof(config);
    config.abi_major = AYNI_ABI_MAJOR;
    config.abi_minor = AYNI_ABI_MINOR;
    config.ingress_capacity = 4;
    config.event_body_max_bytes = 16;
    config.completion_slot_count = 2;
    config.storage_buffer_bytes = AYNI_STORAGE_BUFFER_BYTES_DEFAULT;
    config.agent_capacity = 2;
    config.goal_type_capacity = 2;
    config.goal_capacity = 4;
    config.goal_parameter_max_bytes = 16;
    config.session_capacity = 2;
    config.goal_depth_max = 2;
    config.goal_children_max = 2;
    config.task_capacity = 4;
    config.strategy_capacity = 2;
    config.policy_intent_capacity = 2;
    config.task_step_capacity = 2;
    config.task_checkpoint_max_bytes = 8;
    config.command_capacity = 4;
    config.capability_capacity = AYNI_CAPABILITY_CAPACITY_DEFAULT;
    config.authority_scope_max_bytes = AYNI_AUTHORITY_SCOPE_MAX_BYTES_DEFAULT;
    return config;
}

ayni_host_v1 valid_host() {
    ayni_host_v1 host{};
    host.struct_size = sizeof(host);
    host.monotonic_now_us = &fixed_clock;
    host.wake = &no_wake;
    host.ingress_lock = &no_lock;
    host.ingress_unlock = &no_unlock;
    return host;
}

std::size_t required_size(const ayni_config_v1 &config) {
    std::size_t size = 0;
    std::size_t align = 0;
    EXPECT_EQ(ayni_kernel_memory_requirements(&config, &size, &align), AYNI_OK);
    return size;
}

// Caller-provided kernel memory, over-sized and over-aligned so tests can
// also hand out misaligned or short views of it.
struct KernelMemory {
    alignas(64) std::array<unsigned char, 16384> bytes{};
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

// White-box: the copy is checked through the kernel's own representation.
TEST_F(KernelApi, create_copies_host_table_before_return) {
    const ayni_config_v1 config = valid_config();
    int ctx_target = 0;
    ayni_kernel_t *kernel = nullptr;
    {
        ayni_host_v1 host = valid_host();
        host.ctx = &ctx_target;
        ASSERT_EQ(create(&config, &host, &kernel), AYNI_OK);
        std::memset(&host, 0xA5, sizeof(host));
    }
    EXPECT_EQ(kernel->host.struct_size, sizeof(ayni_host_v1));
    EXPECT_EQ(kernel->host.ctx, &ctx_target);
    ASSERT_EQ(kernel->host.monotonic_now_us, &fixed_clock);
    EXPECT_EQ(kernel->host.monotonic_now_us(kernel->host.ctx), 42U);
    EXPECT_EQ(kernel->host.wake, &no_wake);
    EXPECT_EQ(kernel->host.ingress_lock, &no_lock);
    EXPECT_EQ(kernel->host.ingress_unlock, &no_unlock);
    EXPECT_EQ(kernel->host.storage_append, nullptr);
    EXPECT_EQ(kernel->host.storage_read_prefix, nullptr);
    EXPECT_EQ(ayni_destroy(kernel), AYNI_OK);
}

TEST_F(KernelApi, create_copies_storage_ports) {
    const ayni_config_v1 config = valid_config();
    ayni_host_v1 host = valid_host();
    host.storage_append = &reject_append;
    host.storage_read_prefix = &reject_read;
    ayni_kernel_t *kernel = nullptr;
    ASSERT_EQ(create(&config, &host, &kernel), AYNI_OK);
    EXPECT_EQ(kernel->host.storage_append, &reject_append);
    EXPECT_EQ(kernel->host.storage_read_prefix, &reject_read);
    EXPECT_EQ(ayni_destroy(kernel), AYNI_OK);
}

// ADR-0016: storage is optional, but only as a pair.
TEST_F(KernelApi, create_rejects_only_one_storage_port) {
    const ayni_config_v1 config = valid_config();
    ayni_kernel_t *kernel = nullptr;
    ayni_host_v1 host = valid_host();
    host.storage_append = &reject_append;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT);
    host = valid_host();
    host.storage_read_prefix = &reject_read;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT);
    // A table ending inside storage_read_prefix has storage_append alone.
    host = valid_host();
    host.storage_append = &reject_append;
    host.storage_read_prefix = &reject_read;
    host.struct_size =
        offsetof(ayni_host_v1, storage_read_prefix) + sizeof(host.storage_read_prefix) - 1U;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(kernel, nullptr);
}

// A 0.3 Host table ends before the storage ports; it is still valid, and the
// kernel it creates cannot start (kernel_lifecycle_test.cpp).
TEST_F(KernelApi, host_table_without_storage_ports_is_accepted) {
    const ayni_config_v1 config = valid_config();
    ayni_host_v1 host = valid_host();
    host.storage_append = &reject_append;
    host.storage_read_prefix = &reject_read;
    host.struct_size = offsetof(ayni_host_v1, storage_append);
    ayni_kernel_t *kernel = nullptr;
    ASSERT_EQ(create(&config, &host, &kernel), AYNI_OK);
    EXPECT_EQ(kernel->host.storage_append, nullptr);
    EXPECT_EQ(kernel->host.storage_read_prefix, nullptr);
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

TEST_F(KernelApi, create_rejects_missing_wake_or_ingress_lock) {
    const ayni_config_v1 config = valid_config();
    ayni_kernel_t *kernel = nullptr;
    ayni_host_v1 host = valid_host();
    host.wake = nullptr;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT);
    host = valid_host();
    host.ingress_lock = nullptr;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT);
    host = valid_host();
    host.ingress_unlock = nullptr;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(kernel, nullptr);
}

// A table that ends inside or before the 0.3 ports lacks required ports; the
// partly covered pointer must not be read as one.
TEST_F(KernelApi, host_table_without_0_3_ports_is_invalid_argument) {
    const ayni_config_v1 config = valid_config();
    ayni_kernel_t *kernel = nullptr;
    ayni_host_v1 host = valid_host();
    host.struct_size = offsetof(ayni_host_v1, wake);
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT);
    host.struct_size = offsetof(ayni_host_v1, ingress_unlock) + sizeof(host.ingress_unlock) - 1;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(kernel, nullptr);
}

TEST_F(KernelApi, struct_size_below_first_layout_is_invalid_argument) {
    ayni_config_v1 config = valid_config();
    ayni_host_v1 host = valid_host();
    ayni_kernel_t *kernel = nullptr;

    config.struct_size = offsetof(ayni_config_v1, abi_major) + sizeof(config.abi_major) - 1;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT);
    config = valid_config();
    host.struct_size = offsetof(ayni_host_v1, monotonic_now_us) + sizeof(host.monotonic_now_us) - 1;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT);
}

TEST_F(KernelApi, capacity_outside_its_range_is_invalid_argument) {
    const ayni_host_v1 host = valid_host();
    ayni_kernel_t *kernel = nullptr;
    std::size_t size = 0;
    std::size_t align = 0;
    const struct {
        uint32_t ayni_config_v1::*field;
        uint32_t min;
        uint32_t max;
    } capacities[] = {
        {&ayni_config_v1::ingress_capacity, 1U, AYNI_INGRESS_CAPACITY_MAX},
        {&ayni_config_v1::event_body_max_bytes, 1U, AYNI_EVENT_BODY_MAX_BYTES_MAX},
        {&ayni_config_v1::completion_slot_count, 1U, AYNI_COMPLETION_SLOT_COUNT_MAX},
        {&ayni_config_v1::agent_capacity, 1U, AYNI_AGENT_CAPACITY_MAX},
        {&ayni_config_v1::goal_type_capacity, 1U, AYNI_GOAL_TYPE_CAPACITY_MAX},
        {&ayni_config_v1::goal_capacity, 1U, AYNI_GOAL_CAPACITY_MAX},
        {&ayni_config_v1::goal_parameter_max_bytes, AYNI_GOAL_PARAMETER_MAX_BYTES_MIN,
         AYNI_GOAL_PARAMETER_MAX_BYTES_MAX},
        {&ayni_config_v1::session_capacity, 1U, AYNI_SESSION_CAPACITY_MAX},
        {&ayni_config_v1::goal_depth_max, AYNI_GOAL_DEPTH_MAX_MIN, AYNI_GOAL_DEPTH_MAX_MAX},
        {&ayni_config_v1::goal_children_max, AYNI_GOAL_CHILDREN_MAX_MIN,
         AYNI_GOAL_CHILDREN_MAX_MAX},
        {&ayni_config_v1::task_capacity, 1U, AYNI_TASK_CAPACITY_MAX},
        {&ayni_config_v1::strategy_capacity, 1U, AYNI_STRATEGY_CAPACITY_MAX},
        {&ayni_config_v1::policy_intent_capacity, 1U, AYNI_POLICY_INTENT_CAPACITY_MAX},
        {&ayni_config_v1::task_step_capacity, 1U, AYNI_TASK_STEP_CAPACITY_MAX},
        {&ayni_config_v1::task_checkpoint_max_bytes, AYNI_TASK_CHECKPOINT_MAX_BYTES_MIN,
         AYNI_TASK_CHECKPOINT_MAX_BYTES_MAX},
        {&ayni_config_v1::command_capacity, 1U, AYNI_COMMAND_CAPACITY_MAX},
        {&ayni_config_v1::capability_capacity, 1U, AYNI_CAPABILITY_CAPACITY_MAX},
        {&ayni_config_v1::authority_scope_max_bytes, AYNI_AUTHORITY_SCOPE_MAX_BYTES_MIN,
         AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX},
    };
    for (const auto &capacity : capacities) {
        std::vector<uint32_t> invalid{capacity.max + 1U};
        if (capacity.min > 0U) {
            invalid.push_back(0U);
            invalid.push_back(capacity.min - 1U);
        }
        for (const uint32_t value : invalid) {
            ayni_config_v1 config = valid_config();
            config.*capacity.field = value;
            EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT) << value;
            EXPECT_EQ(ayni_kernel_memory_requirements(&config, &size, &align),
                      AYNI_INVALID_ARGUMENT)
                << value;
        }
        for (const uint32_t value : {capacity.min, capacity.max}) {
            ayni_config_v1 config = valid_config();
            config.*capacity.field = value;
            EXPECT_EQ(ayni_kernel_memory_requirements(&config, &size, &align), AYNI_OK) << value;
        }
    }
    // 0 takes the default; an explicit storage buffer holds the largest batch
    // a transition writes, and at least the minimum (ADR-0026).
    const uint32_t needed = ayni::test::smallest_storage_buffer(valid_config());
    ASSERT_GT(needed, AYNI_STORAGE_BUFFER_BYTES_MIN);
    for (const uint32_t value : {AYNI_STORAGE_BUFFER_BYTES_MIN - 1U, AYNI_STORAGE_BUFFER_BYTES_MIN,
                                 needed - 1U, AYNI_STORAGE_BUFFER_BYTES_MAX + 1U}) {
        ayni_config_v1 config = valid_config();
        config.storage_buffer_bytes = value;
        EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT) << value;
        EXPECT_EQ(ayni_kernel_memory_requirements(&config, &size, &align), AYNI_INVALID_ARGUMENT)
            << value;
    }
    for (const uint32_t value :
         {AYNI_STORAGE_BUFFER_BYTES_DEFAULT, needed, AYNI_STORAGE_BUFFER_BYTES_MAX}) {
        ayni_config_v1 config = valid_config();
        config.storage_buffer_bytes = value;
        EXPECT_EQ(ayni_kernel_memory_requirements(&config, &size, &align), AYNI_OK) << value;
    }
    EXPECT_EQ(kernel, nullptr);
}

// ADR-0027: three batches avoid false fullness when one in-flight batch
// splits the free space into two pieces.
TEST_F(KernelApi, default_storage_buffer_holds_three_of_the_largest_batches) {
    ayni_config_v1 config = valid_config();
    const std::size_t by_default = required_size(config);
    config.storage_buffer_bytes = 3U * ayni::test::smallest_storage_buffer(config);
    EXPECT_EQ(required_size(config), by_default);
}

TEST_F(KernelApi, memory_requirements_grow_with_each_capacity) {
    const std::size_t base = required_size(valid_config());
    ayni_config_v1 config = valid_config();
    config.ingress_capacity += 1;
    EXPECT_GT(required_size(config), base);
    config = valid_config();
    config.event_body_max_bytes += 64;
    EXPECT_GT(required_size(config), base);
    config = valid_config();
    config.completion_slot_count += 1;
    EXPECT_GT(required_size(config), base);
    config = valid_config();
    config.storage_buffer_bytes = 3U * ayni::test::smallest_storage_buffer(config) + 8U;
    EXPECT_GT(required_size(config), base);
    for (uint32_t ayni_config_v1::*field :
         {&ayni_config_v1::agent_capacity, &ayni_config_v1::goal_type_capacity,
          &ayni_config_v1::goal_capacity, &ayni_config_v1::goal_parameter_max_bytes,
          &ayni_config_v1::session_capacity, &ayni_config_v1::task_capacity,
          &ayni_config_v1::strategy_capacity, &ayni_config_v1::policy_intent_capacity,
          &ayni_config_v1::task_step_capacity, &ayni_config_v1::task_checkpoint_max_bytes,
          &ayni_config_v1::command_capacity, &ayni_config_v1::capability_capacity,
          &ayni_config_v1::authority_scope_max_bytes}) {
        config = valid_config();
        config.*field += 1;
        EXPECT_GT(required_size(config), base);
    }
}

// The goal tree limits bound what may be created, not what is stored.
TEST_F(KernelApi, goal_tree_limits_need_no_memory) {
    const std::size_t base = required_size(valid_config());
    ayni_config_v1 config = valid_config();
    config.goal_depth_max = AYNI_GOAL_DEPTH_MAX_MAX;
    config.goal_children_max = AYNI_GOAL_CHILDREN_MAX_MAX;
    EXPECT_EQ(required_size(config), base);
}

// C-VER-003: a field that struct_size does not cover is never read and takes
// its documented default.
// The command field of 0.14 at its default.
ayni_config_v1 with_command_default(ayni_config_v1 config) {
    config.command_capacity = AYNI_COMMAND_CAPACITY_DEFAULT;
    config.capability_capacity = AYNI_CAPABILITY_CAPACITY_DEFAULT;
    config.authority_scope_max_bytes = AYNI_AUTHORITY_SCOPE_MAX_BYTES_DEFAULT;
    return config;
}

// The step and checkpoint fields of 0.12 and the field after them at their
// defaults.
ayni_config_v1 with_step_defaults(ayni_config_v1 config) {
    config.task_step_capacity = AYNI_TASK_STEP_CAPACITY_DEFAULT;
    config.task_checkpoint_max_bytes = AYNI_TASK_CHECKPOINT_MAX_BYTES_DEFAULT;
    return with_command_default(config);
}

// The strategy fields of 0.10 and those after them at their defaults.
ayni_config_v1 with_strategy_defaults(ayni_config_v1 config) {
    config.strategy_capacity = AYNI_STRATEGY_CAPACITY_DEFAULT;
    config.policy_intent_capacity = AYNI_POLICY_INTENT_CAPACITY_DEFAULT;
    return with_step_defaults(config);
}

// The work model fields of 0.5 to 0.14 at their defaults.
ayni_config_v1 with_work_defaults(ayni_config_v1 config) {
    config.agent_capacity = AYNI_AGENT_CAPACITY_DEFAULT;
    config.goal_type_capacity = AYNI_GOAL_TYPE_CAPACITY_DEFAULT;
    config.goal_capacity = AYNI_GOAL_CAPACITY_DEFAULT;
    config.goal_parameter_max_bytes = AYNI_GOAL_PARAMETER_MAX_BYTES_DEFAULT;
    config.session_capacity = AYNI_SESSION_CAPACITY_DEFAULT;
    config.goal_depth_max = AYNI_GOAL_DEPTH_MAX_DEFAULT;
    config.goal_children_max = AYNI_GOAL_CHILDREN_MAX_DEFAULT;
    config.task_capacity = AYNI_TASK_CAPACITY_DEFAULT;
    return with_strategy_defaults(config);
}

TEST_F(KernelApi, uncovered_capacities_take_their_defaults) {
    ayni_config_v1 defaults = with_work_defaults(valid_config());
    defaults.ingress_capacity = AYNI_INGRESS_CAPACITY_DEFAULT;
    defaults.event_body_max_bytes = AYNI_EVENT_BODY_MAX_BYTES_DEFAULT;
    defaults.completion_slot_count = AYNI_COMPLETION_SLOT_COUNT_DEFAULT;
    defaults.storage_buffer_bytes = AYNI_STORAGE_BUFFER_BYTES_DEFAULT;

    ayni_config_v1 short_config = valid_config();
    short_config.struct_size = offsetof(ayni_config_v1, ingress_capacity);
    short_config.ingress_capacity = 0; // would be rejected if it were read
    short_config.storage_buffer_bytes = 0;
    short_config.agent_capacity = 0;
    EXPECT_EQ(required_size(short_config), required_size(defaults));

    // A config ending before storage_buffer_bytes keeps its own capacities.
    ayni_config_v1 only_storage_default = with_work_defaults(valid_config());
    only_storage_default.storage_buffer_bytes = AYNI_STORAGE_BUFFER_BYTES_DEFAULT;
    short_config = valid_config();
    short_config.struct_size = offsetof(ayni_config_v1, storage_buffer_bytes);
    short_config.storage_buffer_bytes = 0;
    short_config.goal_capacity = 0;
    EXPECT_EQ(required_size(short_config), required_size(only_storage_default));

    // So does one ending before the work model fields.
    short_config = valid_config();
    short_config.struct_size = offsetof(ayni_config_v1, agent_capacity);
    short_config.session_capacity = 0;
    EXPECT_EQ(required_size(short_config), required_size(with_work_defaults(valid_config())));

    // One ending before the field of 0.6 takes its default and those after.
    ayni_config_v1 only_task_default = with_strategy_defaults(valid_config());
    only_task_default.task_capacity = AYNI_TASK_CAPACITY_DEFAULT;
    short_config = valid_config();
    short_config.struct_size = offsetof(ayni_config_v1, task_capacity);
    short_config.task_capacity = 0;
    EXPECT_EQ(required_size(short_config), required_size(only_task_default));

    // One ending before the fields of 0.10 takes their defaults and those
    // after.
    short_config = valid_config();
    short_config.struct_size = offsetof(ayni_config_v1, strategy_capacity);
    short_config.strategy_capacity = 0;
    short_config.policy_intent_capacity = 0;
    EXPECT_EQ(required_size(short_config), required_size(with_strategy_defaults(valid_config())));

    // One ending before the fields of 0.12 takes their defaults and the one
    // after.
    short_config = valid_config();
    short_config.struct_size = offsetof(ayni_config_v1, task_step_capacity);
    short_config.task_step_capacity = 0;
    short_config.task_checkpoint_max_bytes = AYNI_TASK_CHECKPOINT_MAX_BYTES_MAX + 1U;
    EXPECT_EQ(required_size(short_config), required_size(with_step_defaults(valid_config())));

    // And one ending before the field of 0.14 takes only its default.
    short_config = valid_config();
    short_config.struct_size = offsetof(ayni_config_v1, command_capacity);
    short_config.command_capacity = 0;
    EXPECT_EQ(required_size(short_config), required_size(with_command_default(valid_config())));
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

TEST_F(KernelApi, create_rejects_other_abi_minor) {
    ayni_config_v1 config = valid_config();
    const ayni_host_v1 host = valid_host();
    ayni_kernel_t *kernel = nullptr;
    config.abi_minor = AYNI_ABI_MINOR + 1;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_UNSUPPORTED_VERSION);
    config.abi_minor = AYNI_ABI_MINOR - 1;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(kernel, nullptr);
}

// A 0.1 caller's config ends before abi_minor; major 0 accepts no other minor.
TEST_F(KernelApi, config_from_abi_0_1_is_unsupported_version) {
    ayni_config_v1 config = valid_config();
    config.struct_size = offsetof(ayni_config_v1, abi_minor);
    const ayni_host_v1 host = valid_host();
    ayni_kernel_t *kernel = nullptr;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_UNSUPPORTED_VERSION);
    std::size_t size = 0;
    std::size_t align = 0;
    EXPECT_EQ(ayni_kernel_memory_requirements(&config, &size, &align), AYNI_UNSUPPORTED_VERSION);
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
