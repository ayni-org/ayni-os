// The validated configuration of a kernel: what ayni_create read from
// ayni_config_v1 and where it placed the bounded stores.

#ifndef AYNI_CORE_KERNEL_KERNEL_CONFIG_H
#define AYNI_CORE_KERNEL_KERNEL_CONFIG_H

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::kernel {

// Validated capacities from ayni_config_v1; they fix the memory layout.
struct KernelCapacities {
    uint32_t ingress_capacity;
    uint32_t event_body_max_bytes;
    uint32_t completion_slot_count;
    uint32_t storage_buffer_bytes;
    uint32_t agent_capacity;
    uint32_t goal_type_capacity;
    uint32_t goal_capacity;
    uint32_t goal_parameter_max_bytes;
    uint32_t session_capacity;
    uint32_t task_capacity;
    uint32_t strategy_capacity;
    uint32_t policy_intent_capacity;
    uint32_t task_step_capacity;
    uint32_t task_checkpoint_max_bytes;
    uint32_t command_capacity; // per principal
    uint32_t capability_capacity;
    uint32_t authority_scope_max_bytes;
    uint32_t grant_capacity;
    uint32_t grant_delegation_depth_max;
    uint32_t goal_requirement_capacity; // Positive requirements per Goal; fixes metadata stride.
};

// Validated limits from ayni_config_v1 that do not change the layout.
struct KernelLimits {
    uint32_t goal_depth_max;
    uint32_t goal_children_max;
    // Neither is 0 once validated: 0 selects the default (ayni.h,
    // StorageBudget::log_max_default()).
    uint32_t storage_write_interval_us = AYNI_STORAGE_WRITE_INTERVAL_US_DEFAULT;
    uint32_t storage_log_max_bytes = 0;
};

// The bounded stores that follow the kernel in the caller's memory block.
struct KernelStorage {
    void *ingress;
    void *completions;
    uint8_t *storage_buffer;
    void *agents;
    void *goal_types;
    void *goals;
    void *sessions;
    void *tasks;
    void *strategies;
    void *decisions;
    void *commands;
    void *capabilities;
    void *grants;
    void *goal_authority; // Goal-owned ceilings/references, one slot per Goal index.
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_KERNEL_CONFIG_H
