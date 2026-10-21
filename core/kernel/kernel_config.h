// The validated configuration of a kernel: what ayni_create read from
// ayni_config_v1 and where it placed the bounded stores.

#ifndef AYNI_CORE_KERNEL_KERNEL_CONFIG_H
#define AYNI_CORE_KERNEL_KERNEL_CONFIG_H

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
};

// Validated limits from ayni_config_v1 that do not change the layout.
struct KernelLimits {
    uint32_t goal_depth_max;
    uint32_t goal_children_max;
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
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_KERNEL_CONFIG_H
