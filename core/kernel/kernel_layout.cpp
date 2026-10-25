#include "kernel/kernel_layout.h"

namespace ayni::kernel {
namespace {

constexpr std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1U) / alignment * alignment;
}

constexpr std::size_t larger(std::size_t a, std::size_t b) {
    return a > b ? a : b;
}

// Places each store after the previous one at its own alignment.
class Placer {
  public:
    explicit Placer(std::size_t start) : end_(start) {}

    std::size_t place(std::size_t alignment, std::size_t size) {
        const std::size_t offset = align_up(end_, alignment);
        end_ = offset + size;
        alignment_ = larger(alignment_, alignment);
        return offset;
    }

    std::size_t end() const { return end_; }
    std::size_t alignment() const { return alignment_; }

  private:
    std::size_t end_;
    std::size_t alignment_ = alignof(ayni_kernel);
};

// Optional call directories allocate only their configured, fixed capacities.
void place_call_catalogs(Placer &placer, const KernelCapacities &capacities, KernelLayout &layout) {
    layout.call_capabilities_offset = placer.place(
        catalog::CallCapabilityRegistry::storage_alignment,
        catalog::CallCapabilityRegistry::storage_size(capacities.call_capability_capacity));
    layout.providers_offset =
        placer.place(catalog::ProviderRegistry::storage_alignment,
                     catalog::ProviderRegistry::storage_size(capacities.provider_capacity));
    layout.bindings_offset =
        placer.place(catalog::BindingRegistry::storage_alignment,
                     catalog::BindingRegistry::storage_size(capacities.binding_capacity));
}

// Optional call runtime storage never consumes Host completion capacity.
void place_call_runtime(Placer &placer, const KernelCapacities &capacities, KernelLayout &layout) {
    layout.calls_offset =
        placer.place(work::CallTable::storage_alignment,
                     work::CallTable::storage_size(
                         {capacities.call_capacity, capacities.authority_scope_max_bytes,
                          capacities.call_input_max_bytes, capacities.call_result_max_bytes}));
    layout.call_runtime_offset = placer.place(
        CallRuntime::storage_alignment,
        CallRuntime::storage_size({capacities.call_capacity, capacities.call_input_max_bytes,
                                   capacities.call_result_max_bytes}));
    layout.provider_completions_offset =
        placer.place(ProviderMailbox::storage_alignment,
                     ProviderMailbox::storage_size(
                         {capacities.call_capacity, capacities.call_result_max_bytes}));
}

// Inventory descriptions and runtime admission stay separately owned in the arena.
void place_resource_catalogs(Placer &placer, const KernelCapacities &capacities,
                             KernelLayout &layout) {
    layout.resource_catalog_offset =
        placer.place(catalog::ResourceRegistry::storage_alignment,
                     catalog::ResourceRegistry::storage_size(capacities.resource_capacity));
    layout.resource_states_offset = placer.place(
        resources::ResourceAdmission::storage_alignment,
        resources::ResourceAdmission::state_storage_size(capacities.resource_capacity));
    layout.resource_requests_offset = placer.place(
        resources::ResourceAdmission::storage_alignment,
        resources::ResourceAdmission::request_storage_size(capacities.resource_request_capacity));
    layout.devices_offset =
        placer.place(catalog::DeviceCatalog::storage_alignment,
                     catalog::DeviceCatalog::device_storage_size(capacities.device_capacity));
    layout.components_offset =
        placer.place(catalog::DeviceCatalog::storage_alignment,
                     catalog::DeviceCatalog::component_storage_size(capacities.component_capacity));
}

// All optional physical execution stores are explicitly sized; zero capacities allocate no tables.
void place_execution_context(Placer &placer, const KernelCapacities &capacities,
                             KernelLayout &layout) {
    layout.execution_catalog_offset =
        placer.place(catalog::ExecutionRegistry::storage_alignment,
                     catalog::ExecutionRegistry::storage_size(
                         capacities.executor_capacity, capacities.execution_binding_capacity));
    layout.permits_offset =
        placer.place(resources::PermitTable::storage_alignment,
                     resources::PermitTable::storage_size(capacities.executor_capacity));
    layout.scenes_offset =
        placer.place(authority::Scenes::storage_alignment,
                     authority::Scenes::storage_size(
                         {capacities.scene_capacity, capacities.authority_scope_max_bytes}));
    layout.observation_sources_offset = placer.place(
        catalog::ObservationSources::storage_alignment,
        catalog::ObservationSources::storage_size(capacities.observation_source_capacity));
    layout.observations_offset = placer.place(
        knowledge::ObservationTable::storage_alignment,
        knowledge::ObservationTable::storage_size(capacities.observation_source_capacity));
}

} // namespace

KernelLayout layout_for(const KernelCapacities &capacities) {
    Placer placer(sizeof(ayni_kernel));
    KernelLayout layout{};
    layout.ingress_offset = placer.place(
        EventIngress::storage_alignment,
        EventIngress::storage_size(capacities.ingress_capacity, capacities.event_body_max_bytes));
    layout.completions_offset =
        placer.place(CompletionPool::storage_alignment,
                     CompletionPool::storage_size(capacities.completion_slot_count));
    // The storage buffer holds bytes only, so it needs no alignment.
    layout.storage_buffer_offset = placer.place(1, capacities.storage_buffer_bytes);
    layout.agents_offset = placer.place(work::AgentTable::storage_alignment,
                                        work::AgentTable::storage_size(capacities.agent_capacity));
    layout.goal_types_offset =
        placer.place(catalog::GoalTypeRegistry::storage_alignment,
                     catalog::GoalTypeRegistry::storage_size(capacities.goal_type_capacity));
    layout.goals_offset =
        placer.place(work::GoalTable::storage_alignment,
                     work::GoalTable::storage_size(capacities.goal_capacity,
                                                   capacities.goal_parameter_max_bytes));
    layout.sessions_offset =
        placer.place(authority::SessionTable::storage_alignment,
                     authority::SessionTable::storage_size(capacities.session_capacity));
    layout.tasks_offset = placer.place(
        work::TaskTable::storage_alignment,
        work::TaskTable::storage_size({capacities.task_capacity, capacities.task_step_capacity,
                                       capacities.task_checkpoint_max_bytes}));
    layout.strategies_offset =
        placer.place(catalog::StrategyRegistry::storage_alignment,
                     catalog::StrategyRegistry::storage_size(capacities.strategy_capacity));
    layout.decisions_offset =
        placer.place(DecisionBuffer::storage_alignment,
                     DecisionBuffer::storage_size({capacities.policy_intent_capacity,
                                                   capacities.task_step_capacity,
                                                   capacities.task_checkpoint_max_bytes}));
    layout.commands_offset =
        placer.place(CommandTable::storage_alignment,
                     CommandTable::storage_size(
                         authority::PrincipalRegistry::capacity, capacities.command_capacity,
                         exact_command_max_bytes(capacities.authority_scope_max_bytes,
                                                 capacities.goal_parameter_max_bytes,
                                                 capacities.goal_requirement_capacity),
                         capacities.grant_capacity));
    layout.capabilities_offset =
        placer.place(catalog::CapabilityRegistry::storage_alignment,
                     catalog::CapabilityRegistry::storage_size(
                         capacities.capability_capacity, capacities.authority_scope_max_bytes));
    layout.grants_offset =
        placer.place(authority::GrantTable::storage_alignment,
                     authority::GrantTable::storage_size({capacities.grant_capacity,
                                                          capacities.authority_scope_max_bytes,
                                                          capacities.grant_delegation_depth_max}));
    layout.goal_authority_offset =
        placer.place(work::GoalAuthorityTable::storage_alignment,
                     work::GoalAuthorityTable::storage_size(
                         {capacities.goal_capacity, capacities.goal_requirement_capacity,
                          capacities.authority_scope_max_bytes}));
    layout.task_authority_offset =
        placer.place(work::TaskAuthorityTable::storage_alignment,
                     work::TaskAuthorityTable::storage_size(
                         {capacities.task_capacity, capacities.goal_requirement_capacity}));
    place_resource_catalogs(placer, capacities, layout);
    place_call_catalogs(placer, capacities, layout);
    place_call_runtime(placer, capacities, layout);
    place_execution_context(placer, capacities, layout);
    layout.size = placer.end();
    layout.alignment = placer.alignment();
    return layout;
}

} // namespace ayni::kernel
