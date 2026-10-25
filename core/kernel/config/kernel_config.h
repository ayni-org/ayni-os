// The validated configuration of a kernel: what ayni_create read from
// ayni_config_v1 and where it placed the bounded stores.

#ifndef AYNI_CORE_KERNEL_CONFIG_KERNEL_CONFIG_H
#define AYNI_CORE_KERNEL_CONFIG_KERNEL_CONFIG_H

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
    uint32_t resource_capacity =
        0; // Resource descriptions/live states; zero disables registration.
    uint32_t resource_request_capacity = 0; // Queued/held groups; zero disables acquisition.
    uint32_t call_capability_capacity = 0;  // Exact execution contracts; zero disables directory.
    uint32_t provider_capacity = 0;         // Trusted callback/offer descriptors; zero disables.
    uint32_t binding_capacity = 0;          // Exact implementation selections; zero disables.
    uint32_t call_capacity = 0; // Retained calls and Provider delivery slots; zero disables.
    uint32_t call_input_max_bytes = AYNI_CALL_INPUT_BYTES_DEFAULT;   // Complete canonical input.
    uint32_t call_result_max_bytes = AYNI_CALL_RESULT_BYTES_DEFAULT; // UTF-8 result stride.
    uint32_t device_capacity = 0;            // Trusted device inventory, disabled by default.
    uint32_t component_capacity = 0;         // Trusted parent links, disabled by default.
    uint32_t executor_capacity = 0;          // Local catalogs and permit ownership slots.
    uint32_t execution_binding_capacity = 0; // Static physical binding constraints.
    uint32_t scene_capacity = 0; // Authority-owned declarations and durable owner preferences.
    uint32_t observation_source_capacity = 0; // Static sources and current-boot samples.
};

// Validated limits from ayni_config_v1 that do not change the layout.
struct KernelLimits {
    uint32_t goal_depth_max;
    uint32_t goal_children_max;
    // Neither is 0 once validated: 0 selects the default (ayni.h,
    // StorageBudget::log_max_default()).
    uint32_t storage_write_interval_us = AYNI_STORAGE_WRITE_INTERVAL_US_DEFAULT;
    uint32_t storage_log_max_bytes = 0;
    // Positive model request ceiling per Task; no arena region is proportional to it.
    uint32_t task_model_call_hard_limit = AYNI_TASK_MODEL_CALL_HARD_LIMIT_DEFAULT;
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
    void *task_authority; // Task-owned admission references, one slot per stable Task slot.
    void *resource_catalog = nullptr; // Catalog-owned descriptions; null iff disabled.
    void *resource_states = nullptr;  // Resource-owned health/occupancy; null iff catalog disabled.
    void *resource_requests = nullptr; // Resource-owned queue/claims; null iff queue disabled.
    void *call_capabilities = nullptr; // Catalog-owned call semantics; null iff disabled.
    void *providers = nullptr;         // Catalog-owned offers/callbacks; null iff disabled.
    void *bindings = nullptr;          // Catalog-owned exact selections; null iff disabled.
    void *calls = nullptr; // Work-owned identities, state and bodies; null iff disabled.
    void *provider_completions = nullptr; // Reliable delivery storage, separate from Host slots.
    void *devices = nullptr;              // DeviceCatalog identities; unused at zero capacity.
    void *components = nullptr;           // DeviceCatalog parent links; unused at zero capacity.
    void *call_runtime = nullptr; // Boot-local I/O ownership plus staging and receipt copies.
    void *observation_sources = nullptr; // Catalog-owned types; unused at zero capacity.
    void *scenes = nullptr;       // Authority-owned declarations/state and canonical byte pools.
    void *observations = nullptr; // Knowledge-owned latest facts; unused at zero capacity.
    void *execution_catalog = nullptr; // Copied executor ports and binding constraints.
    void *permits = nullptr;           // Resource-owned volatile local leases and quarantine links.
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_CONFIG_KERNEL_CONFIG_H
