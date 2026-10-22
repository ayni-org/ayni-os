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
                     CommandTable::storage_size(authority::PrincipalRegistry::capacity,
                                                capacities.command_capacity));
    layout.capabilities_offset =
        placer.place(catalog::CapabilityRegistry::storage_alignment,
                     catalog::CapabilityRegistry::storage_size(
                         capacities.capability_capacity, capacities.authority_scope_max_bytes));
    layout.size = placer.end();
    layout.alignment = placer.alignment();
    return layout;
}

} // namespace ayni::kernel
