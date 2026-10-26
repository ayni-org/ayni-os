#include "kernel/runtime/kernel_layout.h"

#include "kernel/runtime/kernel_state.h"

namespace ayni::kernel {
namespace {

constexpr std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1U) / alignment * alignment;
}

constexpr std::size_t larger(std::size_t a, std::size_t b) {
    return a > b ? a : b;
}

// Places each store after the previous one at its own alignment and records
// its region in the layout.
class Placer {
  public:
    explicit Placer(KernelLayout &layout) : layout_(layout), end_(sizeof(ayni_kernel)) {
        layout_.alignment = alignof(ayni_kernel);
    }

    void place(Store store, std::size_t alignment, std::size_t size) {
        const std::size_t offset = align_up(end_, alignment);
        layout_.regions[static_cast<std::size_t>(store)] = {offset, size, alignment};
        end_ = offset + size;
        layout_.alignment = larger(layout_.alignment, alignment);
    }

    std::size_t end() const { return end_; }

  private:
    KernelLayout &layout_;
    std::size_t end_;
};

// Optional call directories allocate only their configured, fixed capacities.
void place_call_catalogs(Placer &placer, const KernelCapacities &capacities) {
    placer.place(
        Store::call_capabilities, catalog::CallCapabilityRegistry::storage_alignment,
        catalog::CallCapabilityRegistry::storage_size(capacities.call_capability_capacity));
    placer.place(Store::providers, catalog::ProviderRegistry::storage_alignment,
                 catalog::ProviderRegistry::storage_size(capacities.provider_capacity));
    placer.place(Store::bindings, catalog::BindingRegistry::storage_alignment,
                 catalog::BindingRegistry::storage_size(capacities.binding_capacity));
}

// Optional call runtime storage never consumes Host completion capacity.
void place_call_runtime(Placer &placer, const KernelCapacities &capacities) {
    placer.place(Store::calls, work::CallTable::storage_alignment,
                 work::CallTable::storage_size(call_limits(capacities)));
    placer.place(Store::call_runtime, CallRuntime::storage_alignment,
                 CallRuntime::storage_size(call_runtime_limits(capacities)));
    placer.place(Store::provider_completions, ProviderMailbox::storage_alignment,
                 ProviderMailbox::storage_size(provider_mailbox_limits(capacities)));
}

// Inventory descriptions and runtime admission stay separately owned in the arena.
void place_resource_catalogs(Placer &placer, const KernelCapacities &capacities) {
    placer.place(Store::resource_catalog, catalog::ResourceRegistry::storage_alignment,
                 catalog::ResourceRegistry::storage_size(capacities.resource_capacity));
    placer.place(Store::resource_states, resources::ResourceAdmission::storage_alignment,
                 resources::ResourceAdmission::state_storage_size(capacities.resource_capacity));
    placer.place(
        Store::resource_requests, resources::ResourceAdmission::storage_alignment,
        resources::ResourceAdmission::request_storage_size(capacities.resource_request_capacity));
    placer.place(Store::devices, catalog::DeviceCatalog::storage_alignment,
                 catalog::DeviceCatalog::device_storage_size(capacities.device_capacity));
    placer.place(Store::components, catalog::DeviceCatalog::storage_alignment,
                 catalog::DeviceCatalog::component_storage_size(capacities.component_capacity));
}

// All optional physical execution stores are explicitly sized; zero capacities allocate no tables.
void place_execution_context(Placer &placer, const KernelCapacities &capacities) {
    placer.place(Store::execution_catalog, catalog::ExecutionRegistry::storage_alignment,
                 catalog::ExecutionRegistry::storage_size(capacities.executor_capacity,
                                                          capacities.execution_binding_capacity));
    placer.place(Store::permits, resources::PermitTable::storage_alignment,
                 resources::PermitTable::storage_size(capacities.executor_capacity));
    placer.place(Store::executor_completions, ExecutorMailbox::storage_alignment,
                 ExecutorMailbox::storage_size(capacities.executor_capacity));
    placer.place(Store::scenes, authority::Scenes::storage_alignment,
                 authority::Scenes::storage_size(scene_limits(capacities)));
    placer.place(Store::observation_sources, catalog::ObservationSources::storage_alignment,
                 catalog::ObservationSources::storage_size(capacities.observation_source_capacity));
    placer.place(Store::observations, knowledge::ObservationTable::storage_alignment,
                 knowledge::ObservationTable::storage_size(capacities.observation_source_capacity));
}

} // namespace

KernelLayout layout_for(const KernelCapacities &capacities) {
    KernelLayout layout{};
    Placer placer(layout);
    placer.place(
        Store::ingress, EventIngress::storage_alignment,
        EventIngress::storage_size(capacities.ingress_capacity, capacities.event_body_max_bytes));
    placer.place(Store::completions, CompletionPool::storage_alignment,
                 CompletionPool::storage_size(capacities.completion_slot_count));
    // The storage buffer holds bytes only, so it needs no alignment.
    placer.place(Store::storage_buffer, 1, capacities.storage_buffer_bytes);
    placer.place(Store::agents, work::AgentTable::storage_alignment,
                 work::AgentTable::storage_size(capacities.agent_capacity));
    placer.place(Store::goal_types, catalog::GoalTypeRegistry::storage_alignment,
                 catalog::GoalTypeRegistry::storage_size(capacities.goal_type_capacity));
    placer.place(Store::goals, work::GoalTable::storage_alignment,
                 work::GoalTable::storage_size(capacities.goal_capacity,
                                               capacities.goal_parameter_max_bytes));
    placer.place(Store::sessions, authority::SessionTable::storage_alignment,
                 authority::SessionTable::storage_size(capacities.session_capacity));
    placer.place(Store::tasks, work::TaskTable::storage_alignment,
                 work::TaskTable::storage_size(task_limits(capacities)));
    placer.place(Store::strategies, catalog::StrategyRegistry::storage_alignment,
                 catalog::StrategyRegistry::storage_size(capacities.strategy_capacity));
    placer.place(Store::decisions, DecisionBuffer::storage_alignment,
                 DecisionBuffer::storage_size(decision_limits(capacities)));
    placer.place(Store::commands, CommandTable::storage_alignment,
                 CommandTable::storage_size(command_limits(capacities)));
    placer.place(Store::capabilities, catalog::CapabilityRegistry::storage_alignment,
                 catalog::CapabilityRegistry::storage_size(capacities.capability_capacity,
                                                           capacities.authority_scope_max_bytes));
    placer.place(Store::grants, authority::GrantTable::storage_alignment,
                 authority::GrantTable::storage_size(grant_limits(capacities)));
    placer.place(Store::goal_authority, work::GoalAuthorityTable::storage_alignment,
                 work::GoalAuthorityTable::storage_size(goal_authority_limits(capacities)));
    placer.place(Store::task_authority, work::TaskAuthorityTable::storage_alignment,
                 work::TaskAuthorityTable::storage_size(task_authority_limits(capacities)));
    place_resource_catalogs(placer, capacities);
    place_call_catalogs(placer, capacities);
    place_call_runtime(placer, capacities);
    place_execution_context(placer, capacities);
    layout.size = placer.end();
    return layout;
}

KernelStorage storage_at(void *memory, const KernelLayout &layout) {
    KernelStorage storage;
    for (std::size_t index = 0; index < store_count; ++index) {
        const auto store = static_cast<Store>(index);
        storage.set(store, static_cast<uint8_t *>(memory) + layout[store].offset);
    }
    return storage;
}

} // namespace ayni::kernel
