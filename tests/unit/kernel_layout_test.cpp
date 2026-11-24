// Arena audit (ADR-0014, ADR-0051): layout_for gives every store its own
// aligned region of the caller's block, after the kernel and before the end,
// sized with the limits the store's constructor is built with, and
// storage_at hands each store exactly that region. ASan sees the caller's
// block as one allocation, so it cannot catch two stores sharing bytes;
// these checks do.

#include "kernel/runtime/kernel_layout.h"
#include "kernel/runtime/kernel_state.h"
#include "kernel/service/model/store_limits.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using ayni::kernel::KernelCapacities;
using ayni::kernel::KernelLayout;
using ayni::kernel::Store;
using ayni::kernel::store_count;

// The ayni.h defaults: every optional store disabled.
KernelCapacities default_capacities() {
    KernelCapacities capacities{};
    capacities.ingress_capacity = AYNI_INGRESS_CAPACITY_DEFAULT;
    capacities.event_body_max_bytes = AYNI_EVENT_BODY_MAX_BYTES_DEFAULT;
    capacities.completion_slot_count = AYNI_COMPLETION_SLOT_COUNT_DEFAULT;
    capacities.storage_buffer_bytes = AYNI_STORAGE_BUFFER_BYTES_DEFAULT;
    capacities.agent_capacity = AYNI_AGENT_CAPACITY_DEFAULT;
    capacities.goal_type_capacity = AYNI_GOAL_TYPE_CAPACITY_DEFAULT;
    capacities.goal_capacity = AYNI_GOAL_CAPACITY_DEFAULT;
    capacities.goal_parameter_max_bytes = AYNI_GOAL_PARAMETER_MAX_BYTES_DEFAULT;
    capacities.session_capacity = AYNI_SESSION_CAPACITY_DEFAULT;
    capacities.task_capacity = AYNI_TASK_CAPACITY_DEFAULT;
    capacities.strategy_capacity = AYNI_STRATEGY_CAPACITY_DEFAULT;
    capacities.policy_intent_capacity = AYNI_POLICY_INTENT_CAPACITY_DEFAULT;
    capacities.task_step_capacity = AYNI_TASK_STEP_CAPACITY_DEFAULT;
    capacities.task_checkpoint_max_bytes = AYNI_TASK_CHECKPOINT_MAX_BYTES_DEFAULT;
    capacities.command_capacity = AYNI_COMMAND_CAPACITY_DEFAULT;
    capacities.capability_capacity = AYNI_CAPABILITY_CAPACITY_DEFAULT;
    capacities.authority_scope_max_bytes = AYNI_AUTHORITY_SCOPE_MAX_BYTES_DEFAULT;
    capacities.grant_capacity = AYNI_GRANT_CAPACITY_DEFAULT;
    capacities.grant_delegation_depth_max = AYNI_GRANT_DELEGATION_DEPTH_MAX_DEFAULT;
    capacities.goal_requirement_capacity = AYNI_GOAL_REQUIREMENT_CAPACITY_DEFAULT;
    return capacities;
}

// One of everything and the smallest byte bounds, so neighbouring regions
// are as small as they get.
KernelCapacities minimum_capacities() {
    KernelCapacities capacities = default_capacities();
    capacities.ingress_capacity = 1;
    capacities.event_body_max_bytes = 1;
    capacities.completion_slot_count = 1;
    capacities.storage_buffer_bytes = AYNI_STORAGE_BUFFER_BYTES_MIN;
    capacities.agent_capacity = 1;
    capacities.goal_type_capacity = 1;
    capacities.goal_capacity = 1;
    capacities.goal_parameter_max_bytes = AYNI_GOAL_PARAMETER_MAX_BYTES_MIN;
    capacities.session_capacity = 1;
    capacities.task_capacity = 1;
    capacities.strategy_capacity = 1;
    capacities.policy_intent_capacity = 1;
    capacities.task_step_capacity = 1;
    capacities.task_checkpoint_max_bytes = AYNI_TASK_CHECKPOINT_MAX_BYTES_MIN;
    capacities.command_capacity = 1;
    capacities.capability_capacity = 1;
    capacities.authority_scope_max_bytes = AYNI_AUTHORITY_SCOPE_MAX_BYTES_MIN;
    capacities.grant_capacity = 1;
    capacities.grant_delegation_depth_max = AYNI_GRANT_DELEGATION_DEPTH_MAX_MIN;
    capacities.goal_requirement_capacity = 1;
    return capacities;
}

// The defaults with every optional store enabled, as a managed execution
// node configures them.
KernelCapacities every_store_capacities() {
    KernelCapacities capacities = default_capacities();
    capacities.resource_capacity = AYNI_RESOURCE_CAPACITY_DEFAULT;
    capacities.resource_request_capacity = AYNI_RESOURCE_REQUEST_CAPACITY_DEFAULT;
    capacities.call_capability_capacity = 3;
    capacities.provider_capacity = 2;
    capacities.binding_capacity = 3;
    capacities.call_capacity = 4;
    capacities.device_capacity = 2;
    capacities.component_capacity = 3;
    capacities.executor_capacity = 2;
    capacities.execution_binding_capacity = 3;
    capacities.memory_capacity = 4;
    capacities.scene_capacity = 2;
    capacities.observation_source_capacity = 3;
    capacities.observation_ingress_capacity = 2;
    capacities.reflex_capacity = 4;
    capacities.reflex_event_capacity = 8;
    return capacities;
}

std::vector<KernelCapacities> profiles() {
    return {default_capacities(), minimum_capacities(), every_store_capacities()};
}

TEST(KernelLayout, every_store_has_its_own_aligned_region_after_the_kernel) {
    for (const KernelCapacities &capacities : profiles()) {
        const KernelLayout layout = ayni::kernel::layout_for(capacities);
        // Store order is placement order, so each region starting at or
        // after the previous end means no two overlap and none was skipped.
        std::size_t end = sizeof(ayni_kernel);
        for (std::size_t index = 0; index < store_count; ++index) {
            const auto &region = layout[static_cast<Store>(index)];
            SCOPED_TRACE(index);
            EXPECT_GE(region.offset, end);
            ASSERT_NE(region.alignment, 0U);
            EXPECT_EQ(region.alignment & (region.alignment - 1U), 0U);
            EXPECT_EQ(region.offset % region.alignment, 0U);
            EXPECT_EQ(layout.alignment % region.alignment, 0U);
            end = region.offset + region.size;
        }
        EXPECT_EQ(layout.size, end);
        EXPECT_EQ(layout.alignment % alignof(ayni_kernel), 0U);
    }
}

// ADR-0051 decision 4: layout_for sizes each multi-field store from the one
// limits function of config/store_limits.h or kernel_layout.h, the function
// its constructor takes (kernel_state.h, work_model.h), and not otherwise.
TEST(KernelLayout, each_region_is_sized_by_its_stores_limits_function) {
    using namespace ayni::kernel;
    for (const KernelCapacities &capacities : profiles()) {
        const KernelLayout layout = layout_for(capacities);
        EXPECT_EQ(layout[Store::tasks].size,
                  ayni::work::TaskTable::storage_size(task_limits(capacities)));
        EXPECT_EQ(layout[Store::commands].size,
                  CommandTable::storage_size(command_limits(capacities)));
        EXPECT_EQ(layout[Store::grants].size,
                  ayni::authority::GrantTable::storage_size(grant_limits(capacities)));
        EXPECT_EQ(layout[Store::goal_authority].size,
                  ayni::work::GoalAuthorityTable::storage_size(goal_authority_limits(capacities)));
        EXPECT_EQ(layout[Store::task_authority].size,
                  ayni::work::TaskAuthorityTable::storage_size(task_authority_limits(capacities)));
        EXPECT_EQ(layout[Store::calls].size,
                  ayni::work::CallTable::storage_size(call_limits(capacities)));
        EXPECT_EQ(layout[Store::memories].size,
                  ayni::knowledge::MemoryTable::storage_size(memory_limits(capacities)));
        EXPECT_EQ(layout[Store::scenes].size,
                  ayni::authority::Scenes::storage_size(scene_limits(capacities)));
        EXPECT_EQ(layout[Store::decisions].size,
                  DecisionBuffer::storage_size(decision_limits(capacities)));
        EXPECT_EQ(layout[Store::call_runtime].size,
                  CallRuntime::storage_size(call_runtime_limits(capacities)));
        EXPECT_EQ(layout[Store::provider_completions].size,
                  ProviderMailbox::storage_size(provider_mailbox_limits(capacities)));
    }
}

TEST(KernelLayout, storage_at_hands_each_store_its_region) {
    const KernelLayout layout = ayni::kernel::layout_for(every_store_capacities());
    std::vector<uint8_t> block(layout.size);
    const ayni::kernel::KernelStorage storage = ayni::kernel::storage_at(block.data(), layout);
    for (std::size_t index = 0; index < store_count; ++index) {
        const auto store = static_cast<Store>(index);
        EXPECT_EQ(storage[store], block.data() + layout[store].offset) << index;
    }
}

} // namespace
