// The validated configuration of a kernel: what ayni_create read from
// ayni_config_v1 and where it placed the bounded stores.

#ifndef AYNI_CORE_KERNEL_CONFIG_KERNEL_CONFIG_H
#define AYNI_CORE_KERNEL_CONFIG_KERNEL_CONFIG_H

#include <ayni/ayni.h>

#include <array>
#include <cstddef>
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
    uint32_t scene_capacity = 0;  // Authority-owned declarations and durable owner preferences.
    uint32_t memory_capacity = 0; // Whole-Kernel retained facts; zero disables.
    uint32_t memory_body_max_bytes = AYNI_MEMORY_BODY_BYTES_DEFAULT; // Per-record UTF-8 stride.
    uint32_t memory_dependency_capacity = AYNI_MEMORY_DEPENDENCY_CAPACITY_DEFAULT; // Exact lineage.
    uint32_t observation_source_capacity = 0;  // Static sources and current-boot samples.
    uint32_t observation_ingress_capacity = 0; // Optional fixed Host sample FIFO.
    uint32_t business_contract_max_bytes =
        0; // Bytes per original proof; zero with no call catalogs.
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

// The bounded stores that follow the kernel in the caller's memory block, in
// the order layout_for places them (ADR-0014). The comment names each
// store's owner. Every store, including one whose capacity is zero, has an
// entry here, so a new store is added once and layout_for, storage_at and the
// arena test reach it by name (ADR-0051).
enum class Store : uint32_t {
    workspace,            // Kernel-owned synchronous codec candidates, never persisted.
    ingress,              // EventIngress: queued events and their bodies.
    completions,          // CompletionPool: Host completion slots.
    storage_buffer,       // Bytes of the storage I/O; needs no alignment.
    agents,               // AgentTable.
    goal_types,           // GoalTypeRegistry.
    goals,                // GoalTable.
    calendars,            // CalendarTable: civil rules and stable cursors.
    sessions,             // SessionTable.
    tasks,                // TaskTable.
    strategies,           // StrategyRegistry.
    decisions,            // DecisionBuffer: one decision's views, intents and checkpoint.
    commands,             // CommandTable: windows, exact requests and pending revocations.
    capabilities,         // CapabilityRegistry.
    grants,               // GrantTable, with its Gates and both byte regions.
    goal_authority,       // Goal-owned ceilings/references, one slot per Goal index.
    task_authority,       // Task-owned admission references, one slot per stable Task slot.
    resource_catalog,     // ResourceRegistry: immutable descriptions, no live state.
    resource_states,      // ResourceAdmission: live occupancy/health, never persistent.
    resource_requests,    // ResourceAdmission: bounded pending/held resource groups.
    devices,              // DeviceCatalog: copied immutable device identities.
    components,           // DeviceCatalog: copied component-to-device relationships.
    call_capabilities,    // CallCapabilityRegistry: copied execution contracts.
    providers,            // ProviderRegistry: trusted offers and callback descriptors.
    bindings,             // BindingRegistry: immutable exact implementation selections.
    calls,                // CallTable: work-owned identities, state and bodies.
    call_runtime,         // CallRuntime: boot-local I/O ownership, staging and receipt copies.
    provider_completions, // ProviderMailbox: reliable delivery, separate from Host slots.
    execution_catalog,    // ExecutionRegistry: copied executor ports and binding constraints.
    permits,              // PermitTable: volatile local leases and quarantine links.
    executor_completions, // ExecutorMailbox: request lanes and answers per executor.
    scenes,               // Scenes: declarations, saved preferences and canonical byte pools.
    observation_sources,  // ObservationSources: copied source descriptions.
    observations,         // ObservationTable: the current boot's latest sample per source.
    observation_ingress,  // ObservationIngress: copied FIFO and reliable source gap cells.
    memories,             // MemoryTable: owned facts and fixed UTF-8 bodies.
    contexts,             // ContextTable: one file/version/source anchor per Agent.
    provenance,           // ProvenanceTable: exact Goal/Task content sources and erasure state.
    count,                // Not a store: the number of stores.
};

constexpr std::size_t store_count = static_cast<std::size_t>(Store::count);

// Where each store's memory starts. ayni_create fills every entry from the
// layout (storage_at in kernel_layout.h); each entry then has the store's
// storage_size() bytes at its storage_alignment and outlives the store.
// Tests that build only the work model set the entries it reads and leave
// the others null.
class KernelStorage {
  public:
    void *operator[](Store store) const { return memory_[index(store)]; }
    uint8_t *bytes(Store store) const { return static_cast<uint8_t *>(memory_[index(store)]); }
    void set(Store store, void *memory) { memory_[index(store)] = memory; }

  private:
    static constexpr std::size_t index(Store store) { return static_cast<std::size_t>(store); }

    std::array<void *, store_count> memory_{};
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_CONFIG_KERNEL_CONFIG_H
