// Contract of the C ABI in include/ayni/ayni.h: version query, memory
// requirements, create, lifecycle query and destroy. Completion posting is
// covered by completion_pool_test.cpp; start, step, shutdown and kernel info
// by kernel_lifecycle_test.cpp.

#include "kernel_node.h"
#include "kernel_test_probe.h"

#include <ayni/ayni.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using ayni::kernel::KernelTestProbe;

namespace {

// C-VER-007 / ADR-0082: retired draft values and an unrelated unknown kind reject
// before interpreting a body. The test owns these invalid inputs, not a draft API.
TEST(UnsupportedApi, unknown_intents_reject_before_their_bodies_are_read) {
    ayni::test::Node node;
    node.create();
    constexpr ayni_intent_kind_t retired_memory_write = 0x10001u;
    constexpr ayni_intent_kind_t retired_subscription_open = 0x10003u;
    constexpr ayni_intent_kind_t retired_subscription_close = 0x10004u;
    constexpr ayni_intent_kind_t retired_assistant_pause = 0x10005u;
    constexpr ayni_intent_kind_t retired_assistant_resume = 0x10006u;
    constexpr ayni_intent_kind_t unknown_intent = UINT32_MAX;
    for (const auto kind :
         {retired_memory_write, retired_subscription_open, retired_subscription_close,
          retired_assistant_pause, retired_assistant_resume, unknown_intent}) {
        ayni_intent_v1 intent{};
        intent.struct_size = sizeof(intent);
        intent.kind = kind;
        // A null body is safe: no unpublished struct layout may be inspected.
        EXPECT_EQ(ayni_submit_intent(node.kernel(), &intent, nullptr, nullptr), AYNI_UNSUPPORTED);
    }
}

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

// Bounded capacities exercise the complete current query without a frozen arena size.
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
    config.grant_capacity = AYNI_GRANT_CAPACITY_DEFAULT;
    config.grant_delegation_depth_max = AYNI_GRANT_DELEGATION_DEPTH_MAX_DEFAULT;
    config.goal_requirement_capacity = AYNI_GOAL_REQUIREMENT_CAPACITY_DEFAULT;
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

// Caller-provided kernel memory, sized from the current ABI query with one
// alignment unit of slack for misaligned and short-view boundary checks.
struct KernelMemory {
    std::vector<unsigned char> bytes; // Caller allocation follows the current ABI query.
    std::size_t size = 0;
    std::size_t align = 0;
};

class KernelApi : public ::testing::Test {
  protected:
    void SetUp() override {
        const ayni_config_v1 config = valid_config();
        ASSERT_EQ(ayni_kernel_memory_requirements(&config, &memory_.size, &memory_.align), AYNI_OK);
        memory_.bytes.resize(memory_.size + memory_.align);
        ASSERT_LE(memory_.size, memory_.bytes.size());
        ASSERT_EQ(reinterpret_cast<std::uintptr_t>(memory_.bytes.data()) % memory_.align, 0U);
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

// ADR-0104 / ABI 0.55: optional body/group ports are validated before Core reads new fields.
TEST_F(KernelApi, body_descriptors_and_group_ports_have_explicit_layout_and_option_checks) {
    const auto config = valid_config();
    auto host = valid_host();
    ayni_body_store_v1 bodies{};
    bodies.struct_size = sizeof(bodies);
    host.bodies = &bodies;
    ayni_kernel_t *kernel = nullptr;
    --bodies.struct_size;
    EXPECT_NE(create(&config, &host, &kernel), AYNI_OK);
    bodies.struct_size = sizeof(bodies);
    bodies.reserved = AYNI_BODY_STORE_SHARED_RESULTS;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT);
    bodies.reserved = AYNI_BODY_STORE_SHARED_RESULTS << 1U;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT);
    bodies.reserved = 0;
    host.storage_append_many = [](void *, const ayni_storage_batch_v1 *, uint32_t,
                                  ayni_attempt_ref_v1, ayni_completion_t *) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    };
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT);
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
    EXPECT_EQ(KernelTestProbe::host(kernel).struct_size, sizeof(ayni_host_v1));
    EXPECT_EQ(KernelTestProbe::host(kernel).ctx, &ctx_target);
    ASSERT_EQ(KernelTestProbe::host(kernel).monotonic_now_us, &fixed_clock);
    EXPECT_EQ(KernelTestProbe::host(kernel).monotonic_now_us(KernelTestProbe::host(kernel).ctx),
              42U);
    EXPECT_EQ(KernelTestProbe::host(kernel).wake, &no_wake);
    EXPECT_EQ(KernelTestProbe::host(kernel).ingress_lock, &no_lock);
    EXPECT_EQ(KernelTestProbe::host(kernel).ingress_unlock, &no_unlock);
    EXPECT_EQ(KernelTestProbe::host(kernel).storage_append, nullptr);
    EXPECT_EQ(KernelTestProbe::host(kernel).storage_read_prefix, nullptr);
    EXPECT_EQ(ayni_destroy(kernel), AYNI_OK);
}

TEST_F(KernelApi, create_copies_storage_ports) {
    const ayni_config_v1 config = valid_config();
    ayni_host_v1 host = valid_host();
    host.storage_append = &reject_append;
    host.storage_read_prefix = &reject_read;
    ayni_kernel_t *kernel = nullptr;
    ASSERT_EQ(create(&config, &host, &kernel), AYNI_OK);
    EXPECT_EQ(KernelTestProbe::host(kernel).storage_append, &reject_append);
    EXPECT_EQ(KernelTestProbe::host(kernel).storage_read_prefix, &reject_read);
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
    EXPECT_EQ(kernel, nullptr);
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

// ADR-0053: while the major is 0 a struct has only this minor's layout. A
// shorter config or Host table is refused whole, even when the fields it
// lacks have defaults or are optional ports.
TEST_F(KernelApi, struct_size_below_this_layout_is_invalid_argument) {
    const ayni_host_v1 host = valid_host();
    ayni_kernel_t *kernel = nullptr;
    std::size_t bytes = 17;
    std::size_t alignment = 19;
    for (const std::size_t size :
         {offsetof(ayni_config_v1, abi_major) + sizeof(uint32_t) - 1U,
          offsetof(ayni_config_v1, ingress_capacity), offsetof(ayni_config_v1, command_capacity),
          sizeof(ayni_config_v1) - 1U}) {
        ayni_config_v1 config = valid_config();
        config.struct_size = static_cast<uint32_t>(size);
        EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT) << size;
        EXPECT_EQ(ayni_kernel_memory_requirements(&config, &bytes, &alignment),
                  AYNI_INVALID_ARGUMENT)
            << size;
    }
    const ayni_config_v1 config = valid_config();
    for (const std::size_t size :
         {offsetof(ayni_host_v1, monotonic_now_us), offsetof(ayni_host_v1, wake),
          offsetof(ayni_host_v1, storage_append), offsetof(ayni_host_v1, storage_read_log),
          sizeof(ayni_host_v1) - 1U}) {
        ayni_host_v1 short_host = valid_host();
        short_host.struct_size = static_cast<uint32_t>(size);
        EXPECT_EQ(create(&config, &short_host, &kernel), AYNI_INVALID_ARGUMENT) << size;
    }
    EXPECT_EQ(kernel, nullptr);
    EXPECT_EQ(bytes, 17U);
    EXPECT_EQ(alignment, 19U);
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
        {&ayni_config_v1::grant_capacity, 1U, AYNI_GRANT_CAPACITY_MAX},
        {&ayni_config_v1::grant_delegation_depth_max, AYNI_GRANT_DELEGATION_DEPTH_MAX_MIN,
         AYNI_GRANT_DELEGATION_DEPTH_MAX_MAX},
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
          &ayni_config_v1::authority_scope_max_bytes, &ayni_config_v1::grant_capacity,
          &ayni_config_v1::goal_requirement_capacity}) {
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

// abi_major and abi_minor keep their place in every layout, so a caller of
// another minor hears so even when its config has another size; a config
// that ends before abi_minor cannot say its version at all.
TEST_F(KernelApi, config_version_is_judged_before_its_size) {
    ayni_config_v1 config = valid_config();
    config.abi_minor = AYNI_ABI_MINOR - 1U;
    config.struct_size = offsetof(ayni_config_v1, command_capacity);
    const ayni_host_v1 host = valid_host();
    ayni_kernel_t *kernel = nullptr;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_UNSUPPORTED_VERSION);
    std::size_t size = 0;
    std::size_t align = 0;
    EXPECT_EQ(ayni_kernel_memory_requirements(&config, &size, &align), AYNI_UNSUPPORTED_VERSION);
    config.struct_size = sizeof(config) + 8U;
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_UNSUPPORTED_VERSION);
    config.struct_size = offsetof(ayni_config_v1, abi_minor);
    EXPECT_EQ(create(&config, &host, &kernel), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(kernel, nullptr);
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

// ADR-0037 decision 7: the requirement capacity has no zero default.
TEST_F(KernelApi, goal_requirement_capacity_must_be_in_its_range) {
    auto config = valid_config();
    config.goal_requirement_capacity = 0;
    std::size_t bytes = 17;
    std::size_t alignment = 19;
    EXPECT_EQ(ayni_kernel_memory_requirements(&config, &bytes, &alignment), AYNI_INVALID_ARGUMENT);
    config.goal_requirement_capacity = AYNI_GOAL_REQUIREMENT_CAPACITY_MAX + 1U;
    EXPECT_EQ(ayni_kernel_memory_requirements(&config, &bytes, &alignment), AYNI_INVALID_ARGUMENT);
    config.goal_requirement_capacity = AYNI_GOAL_REQUIREMENT_CAPACITY_MAX;
    EXPECT_EQ(ayni_kernel_memory_requirements(&config, &bytes, &alignment), AYNI_OK);
}
namespace {
// C-CALL-016/ADR-0115: a ledger always has a usable independent Provider mailbox beside Calls.
TEST_F(KernelApi, UnresolvedLedgerRequiresCallsAndHonorsItsIndependentCapacityBound) {
    ayni::test::Node node;
    auto config = node.config();
    size_t bytes = 0, alignment = 0;
    config.unresolved_op_capacity = 1;
    EXPECT_EQ(ayni_kernel_memory_requirements(&config, &bytes, &alignment), AYNI_INVALID_ARGUMENT);
    config.call_capacity = 1;
    EXPECT_EQ(ayni_kernel_memory_requirements(&config, &bytes, &alignment), AYNI_OK);
    config.unresolved_op_capacity = AYNI_UNRESOLVED_OP_CAPACITY_MAX + 1U;
    EXPECT_EQ(ayni_kernel_memory_requirements(&config, &bytes, &alignment), AYNI_INVALID_ARGUMENT);
}
} // namespace
